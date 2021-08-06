/**
 * @file gs_haystack.cpp
 * @author Mit Bailey (mitbailey99@gmail.com)
 * @brief 
 * @version See Git tags for version information.
 * @date 2021.08.04
 * 
 * @copyright Copyright (c) 2021
 * 
 */

#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <si446x.h>
#include "gs_haystack.hpp"
#include "meb_debug.hpp"

void *gs_xband_rx_thread(void *args)
{
    global_data_t *global_data = (global_data_t *)args;

    while (global_data->network_data->thread_status > 0)
    {
        // Init RX if not already.
        if (global_data->rx_status == XSTAT_NOT_READY)
        {
            if (rxmodem_init(global_data->rx_modem, uio_get_id("rx_ipcore"), uio_get_id("rx_dma")) > 0)
            {
                global_data->rx_status = XSTAT_INITD;
            }

        }

        // Arm RX if not already.
        if (global_data->rx_status == XSTAT_INITD)
        {
            if (rxmodem_start(global_data->rx_modem) > 0)
            {
                global_data->rx_status = XSTAT_ARMED;
            }
        }

        if (global_data->rx_status != XSTAT_ARMED)
        {
            dbprintlf(RED_FG "X-Band Radio status failure (%d).", (int)global_data->rx_status);
            usleep(5 SEC);
            continue;
        }

        ssize_t buffer_size = rxmodem_receive(global_data->rx_modem);

        uint8_t *buffer = (uint8_t *)malloc(buffer_size * sizeof(char));
        memset(buffer, 0x0, buffer_size);

        ssize_t read_size = 0;
        read_size = rxmodem_read(global_data->rx_modem, buffer, buffer_size);
        if (read_size != buffer_size)
        {
            dbprintlf(RED_FG "Read %d of %d bytes.", read_size, buffer_size);
            continue;
        }

        gs_network_transmit(global_data->network_data, CS_TYPE_DATA, CS_ENDPOINT_CLIENT, buffer, buffer_size);

        free(buffer);
    }

    if (global_data->network_data->thread_status > 0)
    {
        global_data->network_data->thread_status = 0;
    }
    return nullptr;
}

void *gs_network_rx_thread(void *args)
{
    global_data_t *global_data = (global_data_t *)args;
    network_data_t *network_data = global_data->network_data;

    // TODO: Similar, if not identical, to the network functionality in ground_station.
    // Haystack is a network client to the GS Server, and so should be very similar in socketry to ground_station.

    while (network_data->rx_active)
    {
        if (!network_data->connection_ready)
        {
            usleep(5 SEC);
            continue;
        }

        int read_size = 0;

        while (read_size >= 0 && network_data->rx_active)
        {
            char buffer[sizeof(NetworkFrame) * 2];
            memset(buffer, 0x0, sizeof(buffer));

            dbprintlf(BLUE_BG "Waiting to receive...");
            read_size = recv(network_data->socket, buffer, sizeof(buffer), 0);
            dbprintlf("Read %d bytes.", read_size);

            if (read_size > 0)
            {
                dbprintf("RECEIVED (hex): ");
                for (int i = 0; i < read_size; i++)
                {
                    printf("%02x", buffer[i]);
                }
                printf("(END)\n");

                // Parse the data by mapping it to a NetworkFrame.
                NetworkFrame *network_frame = (NetworkFrame *)buffer;

                // Check if we've received data in the form of a NetworkFrame.
                if (network_frame->checkIntegrity() < 0)
                {
                    dbprintlf("Integrity check failed (%d).", network_frame->checkIntegrity());
                    continue;
                }
                dbprintlf("Integrity check successful.");

                global_data->netstat = network_frame->getNetstat();

                // For now, just print the Netstat.
                uint8_t netstat = network_frame->getNetstat();
                dbprintlf(BLUE_FG "NETWORK STATUS");
                dbprintf("GUI Client ----- ");
                ((netstat & 0x80) == 0x80) ? printf(GREEN_FG "ONLINE" RESET_ALL "\n") : printf(RED_FG "OFFLINE" RESET_ALL "\n");
                dbprintf("Roof UHF ------- ");
                ((netstat & 0x40) == 0x40) ? printf(GREEN_FG "ONLINE" RESET_ALL "\n") : printf(RED_FG "OFFLINE" RESET_ALL "\n");
                dbprintf("Roof X-Band ---- ");
                ((netstat & 0x20) == 0x20) ? printf(GREEN_FG "ONLINE" RESET_ALL "\n") : printf(RED_FG "OFFLINE" RESET_ALL "\n");
                dbprintf("Haystack ------- ");
                ((netstat & 0x10) == 0x10) ? printf(GREEN_FG "ONLINE" RESET_ALL "\n") : printf(RED_FG "OFFLINE" RESET_ALL "\n");

                // Extract the payload into a buffer.
                int payload_size = network_frame->getPayloadSize();
                unsigned char *payload = (unsigned char *)malloc(payload_size);
                if (network_frame->retrievePayload(payload, payload_size) < 0)
                {
                    dbprintlf(RED_FG "Error retrieving data.");
                    continue;
                }

                NETWORK_FRAME_TYPE type = network_frame->getType();
                switch (type)
                {
                case CS_TYPE_ACK:
                {
                    dbprintlf(BLUE_FG "Received an ACK frame!");
                    break;
                }
                case CS_TYPE_NACK:
                {
                    dbprintlf(BLUE_FG "Received a NACK frame!");
                    break;
                }
                case CS_TYPE_CONFIG_XBAND:
                {
                    dbprintlf(BLUE_FG "Received an X-Band CONFIG frame!");
                    if (network_frame->getEndpoint() == CS_ENDPOINT_HAYSTACK)
                    {
                        xband_set_data_t *config = (xband_set_data_t *)payload;
                        // adradio_set_tx_lo(global_data->tx_modem, config->LO);
                        // TODO: Figure out how to configure the X-Band radio.
                    }
                    else
                    {
                        dbprintlf("Received a configuration for Roof X-Band!");
                    }
                    break;
                }
                case CS_TYPE_DATA:
                case CS_TYPE_CONFIG_UHF:
                case CS_TYPE_NULL:
                case CS_TYPE_ERROR:
                default:
                {
                    break;
                }
                }
                free(payload);
            }
            else
            {
                break;
            }
        }
        if (read_size == 0)
        {
            dbprintlf(RED_BG "Connection forcibly closed by the server.");
            strcpy(network_data->discon_reason, "SERVER-FORCED");
            network_data->connection_ready = false;
            continue;
        }
        else if (errno == EAGAIN)
        {
            dbprintlf(YELLOW_BG "Active connection timed-out (%d).", read_size);
            strcpy(network_data->discon_reason, "TIMED-OUT");
            network_data->connection_ready = false;
            continue;
        }
        erprintlf(errno);
    }

    network_data->rx_active = false;
    dbprintlf(FATAL "DANGER! NETWORK RECEIVE THREAD IS RETURNING!");

    if (global_data->network_data->thread_status > 0)
    {
        global_data->network_data->thread_status = 0;
    }
    return nullptr;
}