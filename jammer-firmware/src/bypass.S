/*
 * IDF 4.4 / Arduino-ESP32 2.0.17: esp_wifi_80211_tx() menolak frame
 * yang addr2 bukan MAC interface (ESP_ERR_INVALID_ARG 258).
 * Wrapper ini masuk ke fungsi setelah check tersebut, dengan register
 * sama seperti konvensi pemanggil aslinya.
 */
    .text
    .global esp_wifi_80211_tx_bypass

    .literal_position
    .literal .LC0, esp_wifi_80211_tx+0x19

esp_wifi_80211_tx_bypass:
    entry   a1, 48
    extui   a5, a5, 0, 8
    mov.n   a13, a5
    mov.n   a12, a4
    mov.n   a11, a3
    mov.n   a10, a2
    movi.n  a6, 0
    movi.n  a10, 0
    l32r    a13, .LC0
    jx      a13
