#include <stdint.h>
#include <stdbool.h>
#include "pmsis.h"
#include "cpx.h"
#include "bsp/bsp.h"
#include "wifi.h"

void setupWiFi(void) {
    CPXPacket_t txp;
    txp.route.destination = CPX_T_ESP32;
    txp.route.function = CPX_F_WIFI_CTRL;
    txp.route.version = CPX_VERSION;

    WiFiCTRLPacket_t *wifiCtrl = (WiFiCTRLPacket_t *) txp.data;

    // Set a placeholder SSID
    wifiCtrl->cmd = WIFI_CTRL_SET_SSID;
    static char ssid[] = "MESH_PLACEHOLDER";
    memcpy(wifiCtrl->data, ssid, sizeof(ssid));
    txp.dataLength = sizeof(ssid);
    cpxSendPacketBlocking(&txp);

    cpxPrintToConsole(LOG_TO_CRTP, "Placeholder SSID set.\n");

    // Send the connect command
    wifiCtrl->cmd = WIFI_CTRL_WIFI_CONNECT;
    wifiCtrl->data[0] = 0x00; // Station mode
    wifiCtrl->data[1] = 0x00; // Explicitly set station mode
    txp.dataLength = 2;       // Send 2 bytes of data
    cpxSendPacketBlocking(&txp);

    cpxPrintToConsole(LOG_TO_CRTP, "WiFi connect command sent.\n");
}


void start_example(void) {
    // Initialize the GAP8 system
    pi_bsp_init();
    cpxInit();
    cpxEnableFunction(CPX_F_WIFI_CTRL);

    cpxPrintToConsole(LOG_TO_CRTP, "-- Starting Wi-Fi Setup for ESP-MESH --\n");

    // Set up Wi-Fi
    setupWiFi();

    // Keep the program running
    while (1) {
        pi_yield();
    }
}

int main(void) {
    pi_bsp_init();

    // Boost CPU frequency and voltage for stability
    pi_freq_set(PI_FREQ_DOMAIN_FC, 250000000);
    __pi_pmu_voltage_set(PI_PMU_DOMAIN_FC, 1200);

    return pmsis_kickoff((void *)start_example);
}
