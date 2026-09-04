/*
 * radiod SX1262 backend on RadioLib with the PocketOS Linux HAL.
 *
 * Board wiring comes from the environment with T-Display K230 defaults
 * (DOCUMENTED: LILYGO BSP pinmap and DTS; the module parameters TCXO 3.3 V
 * and DC-DC regulator are DOCUMENTED from the vendor launcher's
 * configuration, UNVERIFIED on our units):
 *   POCKETOS_SX1262_SPI    /dev/spidev0.0 (hardware chip select)
 *   POCKETOS_SX1262_RST    5
 *   POCKETOS_SX1262_BUSY   19
 *   POCKETOS_SX1262_DIO1   20
 *   POCKETOS_SX1262_POWER  44   (module supply enable, -1 = none)
 *   POCKETOS_SX1262_TCXO_MV 3300 (0 = crystal, no TCXO)
 *
 * Status: compiles for riscv64; not yet run on hardware.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "airtime.h"
#include "hal_linux.h"
#include "radio_backend.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Sx1262Priv {
    PocketRadioHal *hal;
    Module *module;
    SX1262 *radio;
    char spi_path[64];
    int rst;
    int busy;
    int dio1;
    int power;
    float tcxo_v;
    bool receiving;
};

volatile bool g_irq_flag;

void on_dio1(void)
{
    g_irq_flag = true;
}

int env_int(const char *name, int def)
{
    const char *v = getenv(name);

    return (v && *v) ? atoi(v) : def;
}

uint32_t pin_or_nc(int pin)
{
    return pin < 0 ? RADIOLIB_NC : (uint32_t)pin;
}

int apply_profile(Sx1262Priv *p, const struct radio_profile *prof, char *err, size_t errlen)
{
    int16_t st = p->radio->begin((float)prof->frequency_mhz, (float)prof->bandwidth_khz,
                                 (uint8_t)prof->spreading_factor, (uint8_t)prof->coding_rate,
                                 (uint8_t)prof->sync_word, (int8_t)prof->tx_power_dbm,
                                 (uint16_t)prof->preamble_length, p->tcxo_v, false);

    if (st != RADIOLIB_ERR_NONE) {
        snprintf(err, errlen, "SX1262 begin failed: %d (%s)", st,
                 p->hal->lastError()[0] ? p->hal->lastError() : "check wiring/power");
        return -EIO;
    }
    st = p->radio->setCRC(prof->crc ? 2 : 0);
    if (st != RADIOLIB_ERR_NONE) {
        snprintf(err, errlen, "SX1262 setCRC failed: %d", st);
        return -EIO;
    }
    p->radio->setDio1Action(on_dio1);
    st = p->radio->startReceive();
    if (st != RADIOLIB_ERR_NONE) {
        snprintf(err, errlen, "SX1262 startReceive failed: %d", st);
        return -EIO;
    }
    p->receiving = true;
    return 0;
}

int sx_init(struct radio_backend *b, char *err, size_t errlen)
{
    Sx1262Priv *p = (Sx1262Priv *)calloc(1, sizeof(*p));
    const char *spi = getenv("POCKETOS_SX1262_SPI");

    if (!p) {
        snprintf(err, errlen, "out of memory");
        return -ENOMEM;
    }
    snprintf(p->spi_path, sizeof(p->spi_path), "%s", (spi && *spi) ? spi : "/dev/spidev0.0");
    p->rst = env_int("POCKETOS_SX1262_RST", 5);
    p->busy = env_int("POCKETOS_SX1262_BUSY", 19);
    p->dio1 = env_int("POCKETOS_SX1262_DIO1", 20);
    p->power = env_int("POCKETOS_SX1262_POWER", 44);
    p->tcxo_v = (float)env_int("POCKETOS_SX1262_TCXO_MV", 3300) / 1000.0f;

    p->hal = new PocketRadioHal(p->spi_path, 4000000);
    p->hal->init();
    if (!p->hal->spiReady()) {
        snprintf(err, errlen, "%s", p->hal->lastError());
        delete p->hal;
        free(p);
        return -ENODEV;
    }
    if (p->power >= 0) {
        p->hal->pinMode((uint32_t)p->power, 1);
        p->hal->digitalWrite((uint32_t)p->power, 1);
        p->hal->delay(20);
    }
    p->module = new Module(p->hal, RADIOLIB_NC, pin_or_nc(p->dio1), pin_or_nc(p->rst),
                           pin_or_nc(p->busy));
    p->radio = new SX1262(p->module);
    b->priv = p;
    return 0;
}

void sx_get_caps(struct radio_backend *b, struct radio_caps *caps)
{
    (void)b;
    caps->frequency_min_mhz = 150.0;
    caps->frequency_max_mhz = 960.0;
    caps->tx_power_min_dbm = -9;
    caps->tx_power_max_dbm = 22;
    caps->max_payload = RADIO_MAX_PAYLOAD;
    caps->cad = true;
}

int sx_configure(struct radio_backend *b, const struct radio_profile *prof, char *err, size_t errlen)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;
    int rc = apply_profile(p, prof, err, errlen);

    if (rc == 0) {
        b->profile = *prof;
    }
    return rc;
}

int sx_send(struct radio_backend *b, const uint8_t *data, size_t len, double *airtime_ms,
            char *err, size_t errlen)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;
    const struct radio_profile *prof = &b->profile;
    int16_t st;

    p->radio->clearDio1Action();
    st = p->radio->transmit(data, len);
    *airtime_ms = lora_airtime_ms(prof->spreading_factor, prof->bandwidth_khz, prof->coding_rate,
                                  prof->preamble_length, len, prof->crc, false);
    p->radio->setDio1Action(on_dio1);
    g_irq_flag = false;
    p->radio->startReceive();
    if (st != RADIOLIB_ERR_NONE) {
        snprintf(err, errlen, "SX1262 transmit failed: %d", st);
        return -EIO;
    }
    return 0;
}

int sx_receive(struct radio_backend *b, struct radio_rx_packet *pkt)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;
    size_t len;
    int16_t st;

    p->hal->serviceIrq();
    if (!g_irq_flag) {
        return 0;
    }
    g_irq_flag = false;
    len = p->radio->getPacketLength();
    if (len == 0 || len > sizeof(pkt->data)) {
        p->radio->startReceive();
        return 0;
    }
    st = p->radio->readData(pkt->data, len);
    pkt->len = len;
    pkt->rssi_dbm = p->radio->getRSSI();
    pkt->snr_db = p->radio->getSNR();
    pkt->frequency_error_hz = p->radio->getFrequencyError();
    pkt->timestamp_ms = radio_now_ms();
    p->radio->startReceive();
    if (st == RADIOLIB_ERR_CRC_MISMATCH) {
        return -EBADMSG;
    }
    if (st != RADIOLIB_ERR_NONE) {
        return -EIO;
    }
    return 1;
}

int sx_cad(struct radio_backend *b, bool *activity)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;
    int16_t st;

    p->radio->clearDio1Action();
    st = p->radio->scanChannel();
    p->radio->setDio1Action(on_dio1);
    g_irq_flag = false;
    p->radio->startReceive();
    if (st == RADIOLIB_LORA_DETECTED) {
        *activity = true;
        return 0;
    }
    if (st == RADIOLIB_CHANNEL_FREE) {
        *activity = false;
        return 0;
    }
    return -EIO;
}

int sx_rssi(struct radio_backend *b, double *dbm)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;

    *dbm = p->radio->getRSSI(false);
    return 0;
}

int sx_poll_fd(struct radio_backend *b)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;

    return p->hal->irqFd();
}

void sx_shutdown(struct radio_backend *b)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;

    if (!p) {
        return;
    }
    if (p->radio) {
        p->radio->clearDio1Action();
        p->radio->sleep();
    }
    if (p->power >= 0 && p->hal) {
        p->hal->digitalWrite((uint32_t)p->power, 0);
    }
    delete p->radio;
    delete p->module;
    delete p->hal;
    free(p);
    b->priv = nullptr;
}

} // namespace

extern "C" const struct radio_backend_ops radio_backend_sx1262_ops = {
    "sx1262",       /* name */
    "sx1262",       /* chip */
    sx_init,
    sx_get_caps,
    sx_configure,
    sx_send,
    sx_receive,
    sx_cad,
    sx_rssi,
    nullptr,        /* inject_rx */
    sx_shutdown,
    sx_poll_fd,
};
