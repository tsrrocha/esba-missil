/**
 * @file lcd_i2c.h
 * @brief Driver para LCD 16x4 com módulo I2C PCF8574 (endereço 0x27)
 *
 * Implementação nativa ESP-IDF do protocolo HD44780 via expansor I2C PCF8574.
 * Compatível com displays 16x4 e 20x4.
 *
 * Pinagem do PCF8574 para HD44780:
 *   P0 = RS, P1 = RW, P2 = EN, P3 = Backlight
 *   P4 = D4, P5 = D5, P6 = D6, P7 = D7
 */

#ifndef LCD_I2C_H
#define LCD_I2C_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 *  Configuração de Hardware
 * ──────────────────────────────────────────────────────────────────────────── */
#define LCD_I2C_ADDR        0x27    /**< Endereço I2C do PCF8574              */
#define LCD_I2C_PORT        0       /**< Porta I2C (I2C_NUM_0)               */
#define LCD_I2C_SDA_PIN     21      /**< GPIO para SDA                       */
#define LCD_I2C_SCL_PIN     22      /**< GPIO para SCL — compartilhado com   */
                                    /*   RST do RFID, ver nota no main.c     */
#define LCD_I2C_FREQ_HZ     100000  /**< 100 kHz para máxima compatibilidade */

#define LCD_COLS            16      /**< Colunas do display                  */
#define LCD_ROWS            4       /**< Linhas do display                   */

/* ────────────────────────────────────────────────────────────────────────────
 *  Bits do PCF8574
 * ──────────────────────────────────────────────────────────────────────────── */
#define LCD_BIT_RS          (1 << 0)
#define LCD_BIT_RW          (1 << 1)
#define LCD_BIT_EN          (1 << 2)
#define LCD_BIT_BACKLIGHT   (1 << 3)

/* ────────────────────────────────────────────────────────────────────────────
 *  API Pública
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o barramento I2C e o LCD em modo 4-bit.
 * @return ESP_OK em sucesso.
 */
esp_err_t lcd_init(void);

/**
 * @brief Limpa todo o conteúdo do display.
 */
void lcd_clear(void);

/**
 * @brief Posiciona o cursor na coluna/linha indicada.
 * @param col Coluna (0 a LCD_COLS-1).
 * @param row Linha  (0 a LCD_ROWS-1).
 */
void lcd_set_cursor(uint8_t col, uint8_t row);

/**
 * @brief Escreve uma string a partir da posição atual do cursor.
 * @param str Ponteiro para string terminada em '\0'.
 */
void lcd_print(const char *str);

/**
 * @brief Escreve uma string em uma posição específica, preenchendo com
 *        espaços até o final da linha para limpar resíduos.
 * @param col Coluna (0 a LCD_COLS-1).
 * @param row Linha  (0 a LCD_ROWS-1).
 * @param str String a exibir.
 */
void lcd_print_at(uint8_t col, uint8_t row, const char *str);

#ifdef __cplusplus
}
#endif

#endif /* LCD_I2C_H */
