/**
 * @file rfid_rc522.h
 * @brief Driver nativo ESP-IDF para o leitor RFID MFRC522 via SPI
 *
 * Implementação minimalista focada na detecção de TAGs ISO 14443A (PICC).
 * Suporta leitura de UID de 4 bytes (Single-Size UID) e 7 bytes (Double-Size).
 *
 * Pinagem SPI (VSPI / SPI3):
 *   SCK  = GPIO 18   (Clock)
 *   MISO = GPIO 19   (Master In, Slave Out)
 *   MOSI = GPIO 23   (Master Out, Slave In)
 *   SS   = GPIO  5   (Chip Select / NSS)
 *   RST  = GPIO  4   (Hardware Reset)
 */

#ifndef RFID_RC522_H
#define RFID_RC522_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 *  Configuração de Hardware SPI
 * ──────────────────────────────────────────────────────────────────────────── */
#define RC522_SPI_HOST      SPI2_HOST   /**< VSPI = SPI3 no ESP-IDF v5       */
#define RC522_PIN_SCK       18
#define RC522_PIN_MISO      19
#define RC522_PIN_MOSI      23
#define RC522_PIN_CS        5
#define RC522_PIN_RST       4           /**< Reset via GPIO dedicado         */
#define RC522_SPI_FREQ_HZ   5000000     /**< 5 MHz (máx MFRC522 = 10 MHz)   */

/* ────────────────────────────────────────────────────────────────────────────
 *  Registradores MFRC522 (subset utilizado)
 * ──────────────────────────────────────────────────────────────────────────── */
#define RC522_REG_COMMAND       0x01
#define RC522_REG_COM_IEN       0x02
#define RC522_REG_DIV_IEN       0x03
#define RC522_REG_COM_IRQ       0x04
#define RC522_REG_DIV_IRQ       0x05
#define RC522_REG_ERROR         0x06
#define RC522_REG_STATUS1       0x07
#define RC522_REG_STATUS2       0x08
#define RC522_REG_FIFO_DATA     0x09
#define RC522_REG_FIFO_LEVEL    0x0A
#define RC522_REG_CONTROL       0x0C
#define RC522_REG_BIT_FRAMING   0x0D
#define RC522_REG_COLL          0x0E
#define RC522_REG_MODE          0x11
#define RC522_REG_TX_MODE       0x12
#define RC522_REG_RX_MODE       0x13
#define RC522_REG_TX_CONTROL    0x14
#define RC522_REG_TX_ASK        0x15
#define RC522_REG_CRC_RESULT_H  0x21
#define RC522_REG_CRC_RESULT_L  0x22
#define RC522_REG_MOD_WIDTH     0x24
#define RC522_REG_T_MODE        0x2A
#define RC522_REG_T_PRESCALER   0x2B
#define RC522_REG_T_RELOAD_H    0x2C
#define RC522_REG_T_RELOAD_L    0x2D
#define RC522_REG_AUTO_TEST     0x36
#define RC522_REG_VERSION       0x37

/* ── Comandos MFRC522 ──────────────────────────────────────────────────── */
#define RC522_CMD_IDLE          0x00
#define RC522_CMD_CALC_CRC      0x03
#define RC522_CMD_TRANSCEIVE    0x0C
#define RC522_CMD_SOFT_RESET    0x0F

/* ── Comandos PICC (ISO 14443A) ────────────────────────────────────────── */
#define PICC_CMD_REQA           0x26
#define PICC_CMD_WUPA           0x52
#define PICC_CMD_SEL_CL1        0x93
#define PICC_CMD_SEL_CL2        0x95
#define PICC_CMD_HLTA           0x50

/* ────────────────────────────────────────────────────────────────────────────
 *  Estrutura de resultado
 * ──────────────────────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t uid[10];    /**< UID lido (até 10 bytes para Triple-Size)       */
    uint8_t uid_len;    /**< Tamanho efetivo do UID em bytes                */
} rc522_uid_t;

/* ────────────────────────────────────────────────────────────────────────────
 *  API Pública
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o barramento SPI e o MFRC522.
 * @return ESP_OK em sucesso.
 */
esp_err_t rc522_init(void);

/**
 * @brief Verifica se há uma TAG no campo do leitor.
 * @return true se uma TAG respondeu ao REQA.
 */
bool rc522_picc_is_present(void);

/**
 * @brief Realiza o anti-collision e lê o UID da TAG presente.
 * @param[out] uid Estrutura preenchida com o UID lido.
 * @return ESP_OK em sucesso, ESP_FAIL se não houver TAG ou erro de colisão.
 */
esp_err_t rc522_picc_read_uid(rc522_uid_t *uid);

/**
 * @brief Envia comando HALT para a TAG (encerra comunicação).
 */
void rc522_picc_halt(void);

/**
 * @brief Compara um UID lido com um UID esperado.
 * @param uid     UID lido pelo leitor.
 * @param target  Array com o UID esperado.
 * @param len     Tamanho do UID esperado em bytes.
 * @return true se forem iguais.
 */
bool rc522_uid_match(const rc522_uid_t *uid, const uint8_t *target, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* RFID_RC522_H */
