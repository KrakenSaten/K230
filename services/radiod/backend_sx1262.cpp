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

/* Every transition back into receive mode goes through here so the result
 * is never ignored: p->receiving reflects the last attempt. Returns the
 * RadioLib status. */
int16_t enter_rx(Sx1262Priv *p)
{
    int16_t st;

    p->radio->setDio1Action(on_dio1);
    g_irq_flag = false;
    st = p->radio->startReceive();
    p->receiving = (st == RADIOLIB_ERR_NONE);
    return st;
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
    st = enter_rx(p);
    if (st != RADIOLIB_ERR_NONE) {
        snprintf(err, errlen, "SX1262 startReceive failed: %d", st);
        return -EIO;
    }
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
    /* begin() is the first thing apply_profile does and it puts the whole new
     * radio configuration on the chip; setCRC() and startReceive() come after
     * it and can fail once it has. So a failure here does not mean the radio
     * is untouched, and b->profile is left alone rather than being claimed to
     * describe it. Deciding what to do about that - put the previous settings
     * back, or report the profile as uncertain - is radiod's, in m_configure,
     * because it is the same decision for every backend. */
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
    /* Always try to get back to RX; radiod asks is_receiving() afterwards and
     * reports "error" instead of "rx" if this failed. */
    if (enter_rx(p) != RADIOLIB_ERR_NONE) {
        snprintf(err, errlen, "SX1262 did not re-enter RX after transmit");
    }
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
        return enter_rx(p) == RADIOLIB_ERR_NONE ? 0 : -EIO;
    }
    st = p->radio->readData(pkt->data, len);
    pkt->len = len;
    pkt->rssi_dbm = p->radio->getRSSI();
    pkt->snr_db = p->radio->getSNR();
    pkt->frequency_error_hz = p->radio->getFrequencyError();
    pkt->timestamp_ms = radio_now_ms();
    pkt->mono_ms = radio_mono_ms();
    enter_rx(p); /* result visible through sx_is_receiving() */
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
    enter_rx(p); /* result visible through sx_is_receiving() */
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

/* What this chip can honestly say about the channel, which is less than the
 * datasheet's feature list suggests.
 *
 * RSSI is real: getRSSI(false) is GetRssiInst, the same register read
 * sx_rssi() already does, and it does not move the radio out of receive.
 *
 * A noise floor is not. The SX1262 has no noise measurement; the RSSI read
 * while nothing is being received is the noise floor, but the chip cannot
 * tell us that nothing is being received, so calling that number "noise"
 * would be a guess dressed as a measurement.
 *
 * "Busy" is not either, and this is the one worth being careful about. CAD
 * is the obvious candidate and it is the wrong one twice over. What
 * scanChannel() detects is a LoRa preamble at the modulation parameters
 * currently configured - not FSK, not another spreading factor, and not a
 * packet whose preamble has already passed - so a quiet CAD result is not a
 * quiet channel. And running it takes the radio out of receive and back
 * again, which is not something a passive status read may do to a service
 * that is in the middle of receiving. Turning an RSSI sample into "busy"
 * instead would need a threshold calibrated on this board against this
 * antenna, and nobody has measured one.
 *
 * So activity stays unknown here, and radio.cad remains what it always was:
 * an explicit, active LoRa-preamble check the caller asks for by name and
 * knows the cost of. See docs/api/radio.md; the real busy determination is
 * recorded there as UNRESOLVED. */
int sx_channel(struct radio_backend *b, struct radio_channel *ch)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;

    /* Only while the chip is actually in receive. GetRssiInst answers
     * whatever mode the radio is in, and after a failed startReceive() it is
     * in standby - so the number would be a reading of a receiver that is
     * not receiving. Returning it with rssi_known set would be exactly the
     * fabrication the rest of this struct exists to avoid, and it would come
     * up at the worst moment: the radio is in the error state and a caller
     * is trying to find out what is wrong with it. */
    if (!p->receiving) {
        return 0;   /* zeroed by the caller: nothing is known */
    }
    ch->rssi_known = true;
    ch->rssi_dbm = p->radio->getRSSI(false);
    ch->noise_known = false;
    ch->activity_known = false;
    return 0;
}

int sx_is_receiving(struct radio_backend *b)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;

    return p->receiving ? 1 : 0;
}

int sx_resume_rx(struct radio_backend *b, char *err, size_t errlen)
{
    Sx1262Priv *p = (Sx1262Priv *)b->priv;
    int16_t st = enter_rx(p);

    if (st != RADIOLIB_ERR_NONE) {
        snprintf(err, errlen, "SX1262 startReceive failed: %d", st);
        return -EIO;
    }
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
    /* No asynchronous transmit. RadioLib's transmit() does not return until
     * the packet has left, and replacing it with startTransmit() plus a
     * DIO1-driven completion would change the exact transmit sequence this
     * board's only two successful on-air runs were made with. radiod uses
     * send() for this backend and the daemon is unresponsive for the airtime
     * exactly as it was before - what changed is that the submitting client
     * is no longer made to wait for it. See docs/api/radio.md. */
    nullptr,        /* tx_begin */
    nullptr,        /* tx_poll */
    sx_receive,
    sx_cad,
    sx_rssi,
    sx_channel,
    nullptr,        /* inject_rx */
    sx_shutdown,
    sx_poll_fd,
    sx_is_receiving,
    sx_resume_rx,
    nullptr,        /* debug_set */
};
