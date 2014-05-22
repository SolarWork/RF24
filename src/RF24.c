/*
 Copyright (C) 2011 J. Coliz <maniacbug@ymail.com>

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 version 2 as published by the Free Software Foundation.
 */

#include "nRF24L01.h"
#include "RF24_config.h"
#include "RF24.h"

#define SPI_BITS 8
#define SPI_MODE 0

#define is_rx_fifo_empty() (read_register(FIFO_STATUS) & RX_EMPTY)
#define is_tx_fifo_empty() (read_register(FIFO_STATUS) & TX_EMPTY)
#define enable_radio() gpio_write(enable_pin, GPIO_HIGH)
#define disable_radio() gpio_write(enable_pin, GPIO_LOW)
#define rf24_testRPD() rf24_testCarrierDetect()
#define pipe0_is_set() (pipe0_status & 0x01)
#define auto_ACK_occurred() (pipe0_status & 0x02)

SPIState *spi;
uint8_t enable_pin; /**< "Chip Enable" pin, activates the RX or TX role, unused on rpi */
char *spidevice;
uint32_t spispeed;
uint8_t chip_select; /**< SPI Chip select */
bool wide_band; /* 2Mbs data rate in use? */
bool p_variant; /* False for RF24L01 and TRUE for RF24L01P */
uint8_t payload_len; /**< Fixed size of payloads */
bool ack_payload_available; /**< Whether there is an ack payload waiting */
bool dyn_payloads_set; /**< Whether dynamic payloads are enabled. */ 
uint8_t ack_payload_length; /**< Dynamic size of pending ack payload. */
uint8_t pipe0_status;
uint8_t pipe0_address[5]; /**< Last address set on pipe 0 for reading. */
uint8_t pipe1_address[5];
uint8_t pipe234_lsb[3];
uint8_t transmit_address[5];
uint8_t addr_width;
/****************************************************************************/
  // Minimum ideal SPI bus speed is 2x data rate
  // If we assume 2Mbs data rate and 16Mhz clock, a
  // divider of 4 is the minimum we want.
  // CLK:BUS 8Mhz:2Mhz, 16Mhz:4Mhz, or 20Mhz:5Mhz

static const char rf24_datarate_e_str_0[] PROGMEM = "1MBPS";
static const char rf24_datarate_e_str_1[] PROGMEM = "2MBPS";
static const char rf24_datarate_e_str_2[] PROGMEM = "250KBPS";
static const char * const rf24_datarate_e_str_P[] PROGMEM = {
  rf24_datarate_e_str_0, 
  rf24_datarate_e_str_1, 
  rf24_datarate_e_str_2, 
};
static const char rf24_model_e_str_0[] PROGMEM = "nRF24L01";
static const char rf24_model_e_str_1[] PROGMEM = "nRF24L01+";
static const char * const rf24_model_e_str_P[] PROGMEM = {
  rf24_model_e_str_0, 
  rf24_model_e_str_1, 
};
static const char rf24_crclength_e_str_0[] PROGMEM = "Disabled";
static const char rf24_crclength_e_str_1[] PROGMEM = "8 bits";
static const char rf24_crclength_e_str_2[] PROGMEM = "16 bits";
static const char * const rf24_crclength_e_str_P[] PROGMEM = {
  rf24_crclength_e_str_0, 
  rf24_crclength_e_str_1, 
  rf24_crclength_e_str_2, 
};
static const char rf24_pa_dbm_e_str_0[] PROGMEM = "PA_MIN";
static const char rf24_pa_dbm_e_str_1[] PROGMEM = "PA_GPIO_LOW";
static const char rf24_pa_dbm_e_str_2[] PROGMEM = "PA_GPIO_HIGH";
static const char rf24_pa_dbm_e_str_3[] PROGMEM = "PA_MAX";
static const char * const rf24_pa_dbm_e_str_P[] PROGMEM = { 
  rf24_pa_dbm_e_str_0, 
  rf24_pa_dbm_e_str_1, 
  rf24_pa_dbm_e_str_2, 
  rf24_pa_dbm_e_str_3, 
};

static const uint8_t pipe_addr[] PROGMEM = {
  RX_ADDR_P0, RX_ADDR_P1, RX_ADDR_P2, RX_ADDR_P3, RX_ADDR_P4, RX_ADDR_P5
};
static const uint8_t pipe_payload_len[] PROGMEM = {
  RX_PW_P0, RX_PW_P1, RX_PW_P2, RX_PW_P3, RX_PW_P4, RX_PW_P5
};
static const uint8_t pipe_enable[] PROGMEM = {
  ERX_P0, ERX_P1, ERX_P2, ERX_P3, ERX_P4, ERX_P5
};

uint8_t * reverse_address(uint8_t *address){
  uint8_t i = 0, j = addr_width - 1, temp = 0;
  while (i < j){
    temp = address[i];
    address[i++] = address[j];
    address[j--] = temp;
  }
  return address;
}

/***********************/
/* Register functions  */
/***********************/
uint8_t read_register_bytes(uint8_t reg, uint8_t* buf, uint8_t len) {
  uint8_t status;
  spi_enable(spi);
  spi_transfer(spi, R_REGISTER | (REGISTER_MASK & reg), &status);
  while (len--) *buf++ = spi_transfer(spi, 0xff, NULL);
  spi_disable(spi);
  return status;
}

uint8_t read_register(uint8_t reg) {
  uint8_t result;
  spi_enable(spi);
  spi_transfer(spi, R_REGISTER | (REGISTER_MASK & reg), NULL);
  spi_transfer(spi, 0xff, &result);
  spi_disable(spi);
  return result;
}

uint8_t write_register_bytes(uint8_t reg, const uint8_t* buf, uint8_t len) {
  /* RPi, x86, nRF25L01(+) are all little-endian so no worry about hton/ntoh*/
  uint8_t status;
  spi_enable(spi);
  spi_transfer(spi, W_REGISTER | (REGISTER_MASK & reg), &status);
  while (len--) spi_transfer(spi, *buf++, NULL);
  spi_disable(spi);
  return status;
}

uint8_t write_register(uint8_t reg, uint8_t value) {
  uint8_t status;
  /* printf("write_register(%02x, %02x)\r\n", reg, value); */
  spi_enable(spi);
  spi_transfer(spi, W_REGISTER | (REGISTER_MASK & reg), &status);
  spi_transfer(spi, value, NULL);
  spi_disable(spi);
  return status;
}

/***********************/
/* Payload functions   */
/***********************/
uint8_t write_payload(const void* buf, uint8_t len) {
  uint8_t status;
  const uint8_t* current = (const uint8_t*)buf;
  uint8_t data_len = (len < payload_len ? len : payload_len);
  uint8_t blank_len = dyn_payloads_set ? 0 : payload_len - data_len;
  
  //printf("[Writing %u bytes %u blanks]", data_len, blank_len);
  
  spi_enable(spi);
  spi_transfer(spi, W_TX_PAYLOAD, &status);
  while (data_len--) spi_transfer(spi, *current++, NULL);
  while (blank_len--) spi_transfer(spi, 0, NULL);
  spi_disable(spi);
  return status;
}

uint8_t read_payload(void* buf, uint8_t buf_len, uint8_t payload_len) {
  uint8_t status, data_len = payload_len, blank_len = 0;
  uint8_t* current = (uint8_t*)buf;
  if (buf_len < payload_len){
    data_len = buf_len;
    blank_len = payload_len - buf_len;
  }
  spi_enable(spi);
  spi_transfer(spi, R_RX_PAYLOAD, &status);
  while (data_len--) spi_transfer(spi, 0xff, current++);
  while (blank_len--) spi_transfer(spi, 0xff, NULL);
  spi_disable(spi);
  return status;
}

/***********************/
/* FIFO functions      */
/***********************/
uint8_t flush_rx() {
  uint8_t status;
  spi_enable(spi);
  spi_transfer(spi, FLUSH_RX, &status);
  spi_disable(spi);
  return status;
}

uint8_t flush_tx() {
  uint8_t status;
  spi_enable(spi);
  spi_transfer(spi, FLUSH_TX, &status);
  spi_disable(spi);
  return status;
}

uint8_t get_status() {
  uint8_t status;
  spi_enable(spi);
  spi_transfer(spi, NOP, &status);
  spi_disable(spi);
  return status;
}

void setTXAddress(uint8_t *addr) {
  memcpy(transmit_address, addr, addr_width);
  write_register_bytes(TX_ADDR, reverse_address(transmit_address), addr_width);
}

void toggle_features() {
  spi_enable(spi);
  spi_transfer(spi, ACTIVATE, NULL);
  spi_transfer(spi, ACTIVATE_2, NULL);
  spi_disable(spi);
}

uint8_t get_dyn_payload_len() {
  uint8_t result = 0;
  spi_enable(spi);
  spi_transfer(spi, R_RX_PL_WID, NULL);
  spi_transfer(spi, 0xff, &result);
  spi_disable(spi);
  return result;
}

/* private function for transmitting packet */
void transmit_payload(const void* buf, uint8_t len) {
  /* Set radio to transmit */
  write_register(CONFIG, (read_register(CONFIG) & ~PRIM_RX));
  write_payload(buf, len); /* Write the payload to the TX FIFO */
  enable_radio(); /* Pulse radio on CE pin to TX one packet from FIFO */
  delayMicroseconds(WRITE_DELAY);
  disable_radio();
}

void rf24_setRXAddressOnPipe(uint8_t *address, uint8_t pipe) {
  if (pipe > MAX_PIPE_NUM) return;
  if (pipe == 0){ /* cache pipe0 address as ackWrites overwrite this */
    pipe0_status = PIPE0_SET;
    memcpy(pipe0_address, address, addr_width);
  }
  switch(pipe){ /* For pipes 2-5, only write the last byte */
    case(0):
    case(1): write_register_bytes(pipe_addr[pipe], reverse_address(address), addr_width); break;
    default: write_register_bytes(pipe_addr[pipe], address + (addr_width - 1), 1); break;
  }
  write_register(pipe_payload_len[pipe], payload_len); /* Set payload len and enable */
  write_register(EN_RXADDR, read_register(EN_RXADDR) | pipe_enable[pipe]);
}

bool rf24_setDataRate(rf24_datarate_e speed) {
  uint8_t setup = read_register(RF_SETUP);
  wide_band = FALSE;
  setup &= ~RF_DR; /* Clear DR bits i.e. 1Mbps is 00 */

  switch(speed){
    case(RF24_250KBPS): {
      setup |= RF_DR_250K; /* Set low speed bit */
      break;
    }
    case(RF24_1MBPS): break; /* Already set */
    case(RF24_2MBPS): {
      wide_band = TRUE;
      setup |= RF_DR_2M; /* Set high speed bit */
      break;
    }
    default: return FALSE;
  }
  write_register(RF_SETUP, setup);
  if (setup == read_register(RF_SETUP)) { /* Verify write */
    return TRUE;
  } 
  else {
    wide_band = FALSE;
    return FALSE;
  }
}

rf24_datarate_e rf24_getDataRate() {
  uint8_t dr = read_register(RF_SETUP) & RF_DR; /* Extract DR bits */
  switch(dr){
    case(RF_DR_250K): return RF24_250KBPS;
    case(RF_DR_1M): return RF24_1MBPS;
    case(RF_DR_2M): return RF24_2MBPS;
    default: return RF24_ERROR;
  }
}

void rf24_setPALevel(rf24_pa_dbm_e level) {
  uint8_t setup = read_register(RF_SETUP) & ~RF_PWR; /* Clear RF_PWR bits */
  switch(level){
    case(RF24_PA_MIN): break; /* Already set */
    case(RF24_PA_LOW): setup |= RF_PWR_LOW; break;
    case(RF24_PA_HIGH): setup |= RF_PWR_HIGH; break;
    case(RF24_PA_MAX): /* Fallthrough */
    case(RF24_PA_ERROR): setup |= RF_PWR_MAX; break;
  }
  write_register(RF_SETUP, setup);
}

rf24_pa_dbm_e rf24_getPALevel() {
  uint8_t power = read_register(RF_SETUP) & RF_PWR; /* Extract RF_PWR bits */
  switch(power){
    case(RF_PWR_MAX): return RF24_PA_MAX;
    case(RF_PWR_HIGH): return RF24_PA_HIGH;
    case(RF_PWR_LOW): return RF24_PA_LOW;
    default: return RF24_PA_MIN;
  }
}

void rf24_setCRCLength(rf24_crclength_e length) {
  uint8_t config = read_register(CONFIG) & ~CRC_BITS; /* Clear CRC bits */
  switch(length){
    case(RF24_CRC_DISABLED): break; /* Already set */
    case(RF24_CRC_8): config |= EN_CRC_8; break; /* Enable 8bit CRC */
    case(RF24_CRC_16): config |= EN_CRC_16; break; /* Enable 16bit CRC */
  }
  write_register(CONFIG, config);
}

void rf24_disableCRC() {
  rf24_setCRCLength(RF24_CRC_DISABLED);
}

rf24_crclength_e rf24_getCRCLength() {
  uint8_t config = read_register(CONFIG) & CRC_BITS; /* Extract CRC bits */
  switch(config){
    case(EN_CRC_8): return RF24_CRC_8;
    case(EN_CRC_16): return RF24_CRC_16;
    default: return RF24_CRC_DISABLED;
  }
}

bool isPVariant() {
  return p_variant;
}

void rf24_setRetries(uint8_t delay, uint8_t count) {
 write_register(SETUP_RETR, (delay&0xf)<<ARD | (count&0xf)<<ARC);
}

uint8_t rf24_setAddressWidth(uint8_t address_width){
  if (address_width > MAX_ADDR_WIDTH || address_width < MIN_ADDR_WIDTH) return 0;
  write_register(AW, address_width);
  addr_width = address_width;
  return addr_width;
}

uint8_t rf24_getAddressWidth(){
  return read_register(AW);
}

void rf24_setChannel(uint8_t channel) {
  // TODO: This method could take advantage of the 'wide_band' calculation
  // done in setChannel() to require certain channel spacing.
  write_register(RF_CH, (channel < MAX_CHANNEL ? channel : MAX_CHANNEL));
}

void rf24_setPayloadSize(uint8_t size) {
  payload_len = (size < MAX_PAYLOAD_LEN ? size : MAX_PAYLOAD_LEN);
}

uint8_t rf24_getPayloadSize() {
  return payload_len;
}

uint8_t rf24_init_radio(char *spi_device, uint32_t spi_speed, uint8_t cepin) {
  // Initialize pins
  spidevice = spi_device;
  spispeed = spi_speed;
  enable_pin = cepin;
  chip_select = (strncmp(spidevice, "/dev/spidev0.1", 14) ? 8 : 9);
  gpio_open(enable_pin, GPIO_OUT);

  spi = spi_init(spidevice, SPI_MODE, SPI_BITS, spispeed, chip_select);
  if (spi == NULL) return 0;
  disable_radio();

  // Must allow the radio time to settle else configuration bits will not necessarily stick.
  // This is actually only required following power up but some settling time also appears to
  // be required after resets too. For full coverage, we'll always assume the worst.
  // Enabling 16b CRC is by far the most obvious case if the wrong timing is used - or skipped.
  // Technically we require 4.5ms + 14us as a worst case. We'll just call it 5ms for good measure.
  // WARNING: Delay is based on P-variant whereby non-P *may* require different timing.
  delay(5);
  // Set 1500uS (minimum for 32B payload in ESB@250KBPS) timeouts, to make testing a little easier
  // WARNING: If this is ever lowered, either 250KBS mode with AA is broken or maximum packet
  // sizes must never be used. See documentation for a more complete explanation.
  write_register(SETUP_RETR, ARD_1500u | ARC_15);

  // Restore our default PA level
  rf24_setPALevel(RF24_PA_MAX);

  // Determine if this is a p or non-p RF24 module and then
  // reset our data rate back to default value. This works
  // because a non-P variant won't allow the data rate to
  // be set to 250Kbps.
  if(rf24_setDataRate(RF24_250KBPS)) p_variant = TRUE;
  
  // Then set the data rate to the slowest (and most reliable) speed supported by all
  // hardware.
  rf24_setDataRate(RF24_1MBPS);

  // Initialize CRC and request 2-byte (16bit) CRC
  rf24_setCRCLength(RF24_CRC_16);
  
  // Disable dynamic payloads, to match dyn_payloads_set setting
  write_register(DYNPD, 0);

  // Reset current status
  // Notice reset and flush is the last thing we do
  write_register(STATUS, (RX_DR | TX_DS | MAX_RT));

  // Set up default configuration.  Callers can always change it later.
  // This channel should be universally safe and not bleed over into adjacent
  // spectrum.
  rf24_setChannel(76);

  // Flush buffers
  flush_rx();
  flush_tx();
  return 1;
}


void rf24_resetcfg(){
  write_register(CONFIG, RST_CFG);
}

void rf24_startListening() {
  write_register(CONFIG, (read_register(CONFIG) | PWR_UP | PRIM_RX));
  write_register(STATUS, (RX_DR | TX_DS | MAX_RT));
  /* If PIPE0's addr has been set and then changed by an autoACK, restore it */
  if (PIPE0_SET && PIPE0_AUTO_ACKED) 
    write_register_bytes(RX_ADDR_P0, reverse_address(pipe0_address), addr_width);
  enable_radio();
  delayMicroseconds(130); /* wait for the radio to come up */
}

void rf24_stopListening() {
  disable_radio();
  flush_tx();
  flush_rx();
}

void rf24_powerDown() {
  write_register(CONFIG, (read_register(CONFIG) & ~PWR_UP));
  delayMicroseconds(POWER_DOWN_DELAY);
}

void rf24_powerUp() {
  write_register(CONFIG, (read_register(CONFIG) | PWR_UP));
  delayMicroseconds(POWER_UP_DELAY);
}

bool rf24_available(uint8_t* pipe_num) {
  uint8_t status = get_status();
  bool result = (status & RX_DR);
  if (result) {
    // If the caller wants the pipe number, include that
    if (pipe_num) *pipe_num = (status & RX_P_NO);
    // Handle ack payload receipt
    if (status & TX_DS) write_register(STATUS, TX_DS);
  }
  return result;
}

uint8_t rf24_recv(void* buf, uint8_t len, uint8_t flags) {
  uint8_t payload_len = (dyn_payloads_set ? get_dyn_payload_len() : MAX_PAYLOAD_LEN);
  if (payload_len > MAX_PAYLOAD_LEN){
    flush_rx(); /* Invalid payload needs flushing */
    return -1;
  }
  read_payload(buf, len, payload_len); /* Fetch the payload */
  if (is_rx_fifo_empty()) /* Clear status bit if there are no more payloads */
    write_register(STATUS, RX_DR);
  return payload_len;
}

bool rf24_send(uint8_t *addr, const void* buf, uint8_t len) {
  bool tx_ok = FALSE;
  /* Check if address already set, saves an SPI call */
  if (memcmp(addr, transmit_address, addr_width)) setTXAddress(addr);
  transmit_payload(buf, len);
  rf24_getStatus(&tx_ok, NULL, NULL); /* Only interested in if it was tx'd */
  printf("TX: %s\n", (tx_ok ? "successful" : "failed"));
  return tx_ok;
}

bool rf24_write(const void* buf, uint8_t len) {
  bool result = FALSE;
  transmit_payload(buf, len);

  uint8_t observe_tx;
  uint8_t status;
  uint32_t sent_at = __millis();
  const uint32_t timeout = 500; //ms to wait for timeout
  do
  {
    status = read_register_bytes(OBSERVE_TX, &observe_tx, 1);
    IF_SERIAL_DEBUG(printf("%x", observe_tx));
  }
  while(! (status & (TX_DS | MAX_RT)) && (__millis() - sent_at < timeout));

  bool tx_ok, tx_fail;
  rf24_getStatus(&tx_ok, &tx_fail, &ack_payload_available);
  
  //printf("%u%u%u\r\n", tx_ok, tx_fail, ack_payload_available);

  result = tx_ok;
  IF_SERIAL_DEBUG(printf("%s\n", result ? "...OK." : "...Failed"));

  // Handle the ack packet
  if (ack_payload_available)
  {
    ack_payload_length = get_dyn_payload_len();
    IF_SERIAL_DEBUG(printf("[AckPacket]/"));
    IF_SERIAL_DEBUG(printf("%i\n", ack_payload_length));
  }
  return result;
}

void rf24_getStatus(bool *tx_ok, bool *tx_fail, bool *rx_ready) {
  /* Read the status field and clear the bits in one call*/
  uint8_t status = write_register(STATUS, (RX_DR | TX_DS | MAX_RT));
  if (tx_ok) *tx_ok = status & TX_DS;
  if (tx_fail) *tx_fail = status & MAX_RT;
  if (rx_ready) *rx_ready = status & RX_DR;
}

void rf24_autoACKPacket(){
    write_register_bytes(RX_ADDR_P0, reverse_address(transmit_address), addr_width);
    write_register(RX_PW_P0, (payload_len < MAX_PAYLOAD_LEN ? payload_len : MAX_PAYLOAD_LEN));
    pipe0_status |= PIPE0_AUTO_ACKED;
}

void rf24_enableDynamicPayloads() {
  /* Enable dynamic payload feature */
  uint8_t status = read_register(FEATURE);
  if ((status & EN_DPL) == 0){
    write_register(FEATURE, (status | EN_DPL));
    printf("Enabling dyn payloads\n");
    // If it didn't work, the features are not enabled
    if (read_register(FEATURE) == 0) {
      toggle_features(); /* So enable them and try again */
      write_register(FEATURE, EN_DPL);
    }
  } /* Already enabled */
  /* Enable dynamic payloads on all pipes */
  write_register(DYNPD, DPL_ALL);
  dyn_payloads_set = TRUE;
  payload_len = 32;
}

void rf24_enableAckPayload() {
  /* enable ack payload and dynamic payload features */
  uint8_t status = read_register(FEATURE);
  if ((status & (EN_ACK_PAY | EN_DPL)) == 0){
    write_register(FEATURE, (status | EN_ACK_PAY | EN_DPL));
    /* If it didn't work, the features are not enabled */
    if (read_register(FEATURE) == 0) {
      toggle_features(); /* So enable them and try again */
      write_register(FEATURE, (EN_ACK_PAY | EN_DPL));
    }
  }
  IF_SERIAL_DEBUG(printf("FEATURE=%i\r\n", read_register(FEATURE)));
  /* Enable dynamic payload on pipes 0 */
  write_register(DYNPD, (read_register(DYNPD) | DPL_P0));
}

void rf24_writeAckPayload(uint8_t pipe, const void* buf, uint8_t len) {
  const uint8_t* current = (const uint8_t*)buf;
  uint8_t data_len = (len < MAX_PAYLOAD_LEN ? len : MAX_PAYLOAD_LEN);
  spi_enable(spi);
  spi_transfer(spi, W_ACK_PAYLOAD | (pipe & 0b111), NULL);
  while (data_len--) spi_transfer(spi, *current++, NULL);
  spi_disable(spi);
}

bool rf24_isAckPayloadAvailable() {
  bool result = ack_payload_available;
  ack_payload_available = FALSE;
  return result;
}

void rf24_setAutoAckOnAll(bool enable) {
  if (enable) write_register(EN_AA, ENAA_ALL);
  else write_register(EN_AA, ENAA_NONE);
}

void rf24_setAutoAckOnPipe(uint8_t pipe, bool enable) {
  if (pipe > 5) return;
  uint8_t en_aa = read_register(EN_AA);
  switch(enable){
    case(TRUE): en_aa |= (1 << pipe); break;
    case(FALSE): en_aa &= (1 << pipe); break;
  }
  write_register(EN_AA, en_aa);
}

bool rf24_testCarrierDetect() {
  return (read_register(CD) & CD_CMD);
}

void print_status(uint8_t status) {
  printf("STATUS\t\t = 0x%02x RX_DR=%x TX_DS=%x MAX_RT=%x RX_P_NO=%x TX_FULL=%x\r\n", 
           status, 
           (status & RX_DR)?1:0, 
           (status & TX_DS)?1:0, 
           (status & MAX_RT)?1:0, 
           ((status >> RX_P_NO) & 0b111), 
           (status & TX_FIFO_FULL)?1:0
         );
}

void print_observe_tx(uint8_t value) {
  printf("OBSERVE_TX=%02x: POLS_CNT=%x ARC_CNT=%x\r\n", 
           value, 
           (value >> PLOS_CNT) & 0b1111, 
           (value >> ARC_CNT) & 0b1111
         );
}

void print_byte_register(char* name, uint8_t reg) {
  printf("\t%s =", name);
  printf(" 0x%x", read_register(reg++));
}

void print_address_register(char* name, uint8_t reg, uint8_t qty) {
  printf("\t%s =", name);
  while (qty--) {
    uint8_t buffer[5];
    read_register_bytes(reg++, buffer, sizeof(buffer));
    printf(" 0x");
    uint8_t* bufptr = buffer + sizeof(buffer);
    while(--bufptr >= buffer) printf("%02x", *bufptr);
  }

  printf("\r\n");
}

void rf24_printDetails() {
  printf("SPI device\t = %s\r\n", spidevice);
  printf("SPI speed\t = %d\r\n", spispeed);
  printf("CE GPIO\t = %d\r\n", enable_pin);
  printf("Data Rate\t = %s\r\n", pgm_read_word(&rf24_datarate_e_str_P[rf24_getDataRate()]));
  printf("Model\t\t = %s\r\n", pgm_read_word(&rf24_model_e_str_P[isPVariant()]));
  printf("CRC Length\t = %s\r\n", pgm_read_word(&rf24_crclength_e_str_P[rf24_getCRCLength()]));
  printf("PA Power\t = %s\r\n", pgm_read_word(&rf24_pa_dbm_e_str_P[rf24_getPALevel()]));
  print_status(get_status());
  print_address_register("RX_ADDR_P0-1", RX_ADDR_P0, 2);
  print_byte_register("RX_ADDR_P2-5", RX_ADDR_P2);
  //print_address_register("TX_ADDR", TX_ADDR);
  print_byte_register("RX_PW_P0-6", RX_PW_P0);
  print_byte_register("EN_AA", EN_AA);
  print_byte_register("EN_RXADDR", EN_RXADDR);
  print_byte_register("RF_CH", RF_CH);
  print_byte_register("RF_SETUP", RF_SETUP);
  print_byte_register("CONFIG", CONFIG);
  print_byte_register("DYNPD/FEATURE", DYNPD);
}