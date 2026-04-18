/**
 * @file lcd_i2c.c
 * @brief Implementação do driver LCD 20x4/16x2 via I2C (PCF8574 + HD44780)
 *
 * O LCD HD44780 é controlado em modo 4-bit através do expansor I2C PCF8574.
 * Cada byte enviado ao PCF8574 mapeia diretamente os 8 pinos do expansor:
 *   Nibble alto (P7-P4) = dados D7-D4 do LCD
 *   P3 = Backlight, P2 = Enable, P1 = RW, P0 = RS
 *
 * O protocolo consiste em:
 *   1. Montar o byte com nibble de dados + bits de controle
 *   2. Pulsar o pino Enable (EN alto -> EN baixo) para latch
 *   3. Para um byte completo, enviar primeiro o nibble alto, depois o baixo
 */

#include "lcd_i2c.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"   /* ets_delay_us() para delays sub-milissegundo */

static const char *TAG = "LCD_I2C";

/* ----------------------------------------------------------------------------
 *  Handles I2C (ESP-IDF v5.x new driver)
 * ---------------------------------------------------------------------------- */
static i2c_master_bus_handle_t  s_bus_handle = NULL;
static i2c_master_dev_handle_t  s_dev_handle = NULL;
static uint8_t s_backlight = LCD_BIT_BACKLIGHT;  /**< Estado do backlight */

/* ----------------------------------------------------------------------------
 *  Funcoes internas
 * ---------------------------------------------------------------------------- */

/**
 * @brief Número máximo de tentativas de envio I2C antes de desistir.
 *
 * Cada tentativa falhada aguarda LCD_I2C_RETRY_DELAY_MS antes de tentar
 * novamente. Isso é útil quando o módulo LCD demora para inicializar
 * ou há mau contato nos fios I2C.
 */
#define LCD_I2C_MAX_RETRIES     5
#define LCD_I2C_RETRY_DELAY_MS  5000

/**
 * @brief Envia um byte cru ao PCF8574 via I2C com retry automático.
 *
 * Se a transmissão falhar (NACK, timeout, etc.), aguarda 5 segundos e
 * tenta novamente até LCD_I2C_MAX_RETRIES vezes. Isso permite que o
 * sistema se recupere de:
 *   - Mau contato temporário nos fios SDA/SCL
 *   - LCD ainda inicializando após power-on
 *   - Interferência eletromagnética momentânea
 */
static esp_err_t pcf8574_write(uint8_t data)
{
    esp_err_t ret;

    for (int attempt = 0; attempt < LCD_I2C_MAX_RETRIES; attempt++) {
        ret = i2c_master_transmit(s_dev_handle, &data, 1, 100);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "I2C NACK/erro (tentativa %d/%d) — aguardando %dms...",
                 attempt + 1, LCD_I2C_MAX_RETRIES, LCD_I2C_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(LCD_I2C_RETRY_DELAY_MS));
    }

    ESP_LOGE(TAG, "I2C falhou após %d tentativas — verifique conexões!",
             LCD_I2C_MAX_RETRIES);
    return ret;
}

/**
 * @brief Pulsa o pino Enable do HD44780.
 *
 * O HD44780 faz latch dos dados na borda de descida do Enable.
 * Tempo mínimo de pulso: 450ns (usamos 1µs por segurança).
 */
static void lcd_pulse_enable(uint8_t data)
{
    pcf8574_write(data | LCD_BIT_EN);   /* EN = 1 */
    ets_delay_us(10);                   /* Aumentado para 10µs para maxima estabilidade */
    pcf8574_write(data & ~LCD_BIT_EN);  /* EN = 0 */
    ets_delay_us(150);                  /* Tempo de processamento estendido (seguro para clones) */
}

/**
 * @brief Envia um nibble (4 bits) ao LCD.
 * @param nibble Nibble no campo alto (bits 7-4).
 * @param mode   0 = comando, LCD_BIT_RS = dados.
 */
static void lcd_send_nibble(uint8_t nibble, uint8_t mode)
{
    uint8_t data;
#if (LCD_DATA_SHIFT >= 0)
    data = (nibble << LCD_DATA_SHIFT) | mode | s_backlight;
#else
    data = (nibble >> (-LCD_DATA_SHIFT)) | mode | s_backlight;
#endif
    pcf8574_write(data);
    lcd_pulse_enable(data);
}

/**
 * @brief Envia um byte completo ao LCD (dois nibbles).
 * @param byte Byte a enviar.
 * @param mode 0 = comando, LCD_BIT_RS = dados (caractere).
 */
static void lcd_send_byte(uint8_t byte, uint8_t mode)
{
    lcd_send_nibble(byte & 0xF0, mode);         /* Nibble alto */
    ets_delay_us(200);                          /* Delay entre nibbles aumentado */
    lcd_send_nibble((byte << 4) & 0xF0, mode);  /* Nibble baixo */
    vTaskDelay(pdMS_TO_TICKS(1));               /* Pequeno folego entre bytes */
}

/**
 * @brief Envia um comando ao LCD.
 */
static void lcd_command(uint8_t cmd)
{
    lcd_send_byte(cmd, 0);
}

/**
 * @brief Envia um caractere ao LCD.
 */
static void lcd_data(uint8_t ch)
{
    lcd_send_byte(ch, LCD_BIT_RS);
}

/* ----------------------------------------------------------------------------
 *  API Publica
 * ---------------------------------------------------------------------------- */

esp_err_t lcd_init(void)
{
    ESP_LOGI(TAG, "Inicializando I2C bus (SDA=%d, SCL=%d)",
             LCD_I2C_SDA_PIN, LCD_I2C_SCL_PIN);

    /* -- Configuracao do barramento I2C (ESP-IDF v5.x new driver) -- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = LCD_I2C_PORT,
        .sda_io_num = LCD_I2C_SDA_PIN,
        .scl_io_num = LCD_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus_handle));

    /* -- Adicao do dispositivo LCD (PCF8574) -- */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = LCD_I2C_ADDR,
        .scl_speed_hz    = LCD_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle));

    /* Teste de comunicacao I2C (ping) */
    if (i2c_master_probe(s_bus_handle, LCD_I2C_ADDR, 100) != ESP_OK) {
        ESP_LOGE(TAG, "LCD nao detectado no endereco 0x%02X! Verifique conexoes.", LCD_I2C_ADDR);
    }

    /* Sequência de inicialização HD44780 robusta (datasheet, Figura 24) */
    vTaskDelay(pdMS_TO_TICKS(100));  /* Espera inicial estendida */

    /* Enviar 0x30 três vezes para garantir sincronismo */
    lcd_send_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_send_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    /* Trocar para modo 4-bit */
    lcd_send_nibble(0x20, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    /* Configuração: 4-bit, 2+ linhas, fonte 5x8 */
    lcd_command(0x28);
    ets_delay_us(100);

    /* Display ON, cursor OFF, blink OFF */
    lcd_command(0x0C);
    ets_delay_us(100);

    /* Clear display */
    lcd_command(0x01);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Entry mode */
    lcd_command(0x06);
    ets_delay_us(100);

    ESP_LOGI(TAG, "LCD %dx%d inicializado com sucesso", LCD_COLS, LCD_ROWS);
    return ESP_OK;
}

void lcd_clear(void)
{
    lcd_command(0x01);
    vTaskDelay(pdMS_TO_TICKS(3));
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    /*
     * Endereços DDRAM típicos:
     *   L0: 0x00 | L1: 0x40 | L2: 0x14 | L3: 0x54 (20x4)
     *   L0: 0x00 | L1: 0x40 | L2: 0x10 | L3: 0x50 (16x4)
     */
    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    if (row >= LCD_ROWS) row = LCD_ROWS - 1;
    if (col >= LCD_COLS) col = LCD_COLS - 1;

    lcd_command(0x80 | (row_offsets[row] + col));
}

void lcd_print(const char *str)
{
    while (*str) {
        lcd_data((uint8_t)*str++);
    }
}

void lcd_print_at(uint8_t col, uint8_t row, const char *str)
{
    lcd_set_cursor(col, row);

    uint8_t pos = col;
    while (*str && pos < LCD_COLS) {
        lcd_data((uint8_t)*str++);
        pos++;
    }
    /* Preencher com espaços para limpar resíduos */
    while (pos < LCD_COLS) {
        lcd_data(' ');
        pos++;
    }
}
