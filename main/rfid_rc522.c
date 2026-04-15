/**
 * @file rfid_rc522.c
 * @brief Implementação do driver MFRC522 via SPI nativo ESP-IDF
 *
 * O MFRC522 comunica-se em modo SPI Half-Duplex:
 *   - Byte 1: Endereço do registrador (bit 7 = 1 para leitura, 0 para escrita)
 *   - Byte 2+: Dados
 *
 * Fluxo para detecção de TAG ISO 14443A:
 *   1. REQA (Request A): TX curto de 7 bits → se ATQA recebido, há TAG
 *   2. Anti-Collision (SELECT CL1): Resolve conflitos → obtém UID de 4 bytes
 *   3. SELECT: Seleciona a TAG para operações subsequentes
 *   4. HALT: Encerra a sessão com a TAG
 */

#include "rfid_rc522.h"

#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

static const char *TAG = "RC522";
static spi_device_handle_t s_spi_handle = NULL;

/* ────────────────────────────────────────────────────────────────────────────
 *  Operações SPI de baixo nível
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Escreve um valor em um registrador do MFRC522.
 *
 * Protocolo SPI MFRC522 para escrita:
 *   Byte 0: (reg << 1) & 0x7E  → bit 7 = 0 (escrita)
 *   Byte 1: valor a escrever
 */
static void rc522_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {(reg << 1) & 0x7E, value};
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
    };
    spi_device_transmit(s_spi_handle, &t);
}

/**
 * @brief Lê um registrador do MFRC522.
 *
 * Protocolo SPI MFRC522 para leitura:
 *   Byte 0: ((reg << 1) & 0x7E) | 0x80  → bit 7 = 1 (leitura)
 *   Byte 1: 0x00 (dummy, recebemos o valor aqui)
 */
static uint8_t rc522_read_reg(uint8_t reg)
{
    uint8_t tx[2] = {((reg << 1) & 0x7E) | 0x80, 0x00};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(s_spi_handle, &t);
    return rx[1];
}

/**
 * @brief Define bits em um registrador (OR lógico).
 */
static void rc522_set_bits(uint8_t reg, uint8_t mask)
{
    rc522_write_reg(reg, rc522_read_reg(reg) | mask);
}

/**
 * @brief Limpa bits em um registrador (AND NOT).
 */
static void rc522_clear_bits(uint8_t reg, uint8_t mask)
{
    rc522_write_reg(reg, rc522_read_reg(reg) & ~mask);
}

/* ────────────────────────────────────────────────────────────────────────────
 *  Comunicação com PICC (TAG)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Executa o comando Transceive: envia dados pela antena e recebe resposta.
 *
 * @param tx_data   Dados a transmitir.
 * @param tx_len    Número de bytes a transmitir.
 * @param rx_data   Buffer para dados recebidos (pode ser NULL).
 * @param rx_len    [in/out] Tamanho do buffer / bytes recebidos.
 * @param tx_last_bits Número de bits válidos no último byte TX (0 = 8 bits).
 * @return ESP_OK se comunicação bem-sucedida, ESP_ERR_TIMEOUT se sem resposta.
 */
static esp_err_t rc522_transceive(const uint8_t *tx_data, uint8_t tx_len,
                                   uint8_t *rx_data, uint8_t *rx_len,
                                   uint8_t tx_last_bits)
{
    /* Idle antes de novo comando */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);

    /* Limpar flags de interrupção */
    rc522_write_reg(RC522_REG_COM_IRQ, 0x7F);

    /* Flush FIFO */
    rc522_set_bits(RC522_REG_FIFO_LEVEL, 0x80);

    /* Carregar dados no FIFO */
    for (uint8_t i = 0; i < tx_len; i++) {
        rc522_write_reg(RC522_REG_FIFO_DATA, tx_data[i]);
    }

    /* Configurar bits do último byte */
    rc522_write_reg(RC522_REG_BIT_FRAMING, tx_last_bits & 0x07);

    /* Executar Transceive */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_TRANSCEIVE);

    /* Iniciar transmissão (StartSend bit) */
    rc522_set_bits(RC522_REG_BIT_FRAMING, 0x80);

    /* Polling para conclusão (timeout ~25ms) */
    uint16_t wait = 2500;
    uint8_t irq;
    do {
        irq = rc522_read_reg(RC522_REG_COM_IRQ);
        wait--;
    } while (wait > 0 && !(irq & 0x30));  /* RxIRq (bit5) ou IdleIRq (bit4) */

    /* Parar transmissão */
    rc522_clear_bits(RC522_REG_BIT_FRAMING, 0x80);

    if (wait == 0) {
        return ESP_ERR_TIMEOUT;
    }

    /* Verificar erros */
    uint8_t error = rc522_read_reg(RC522_REG_ERROR);
    if (error & 0x1B) {  /* BufferOvfl, CollErr, ParityErr, ProtocolErr */
        ESP_LOGD(TAG, "Erro de comunicação: 0x%02X", error);
        return ESP_FAIL;
    }

    /* Ler dados recebidos do FIFO */
    if (rx_data && rx_len) {
        uint8_t n = rc522_read_reg(RC522_REG_FIFO_LEVEL);
        if (n > *rx_len) n = *rx_len;
        *rx_len = n;
        for (uint8_t i = 0; i < n; i++) {
            rx_data[i] = rc522_read_reg(RC522_REG_FIFO_DATA);
        }
    }

    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 *  API Pública
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t rc522_init(void)
{
    ESP_LOGI(TAG, "Inicializando SPI bus e MFRC522...");

    /* ── Configuração do barramento SPI ────────────────────────────────── */
    spi_bus_config_t bus_cfg = {
        .miso_io_num   = RC522_PIN_MISO,
        .mosi_io_num   = RC522_PIN_MOSI,
        .sclk_io_num   = RC522_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(RC522_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED));

    /* ── Adição do dispositivo MFRC522 ─────────────────────────────────── */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = RC522_SPI_FREQ_HZ,
        .mode           = 0,                    /* CPOL=0, CPHA=0 */
        .spics_io_num   = RC522_PIN_CS,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(RC522_SPI_HOST, &dev_cfg, &s_spi_handle));

    /* ── Hardware Reset ────────────────────────────────────────────────── */
    gpio_reset_pin(RC522_PIN_RST);
    gpio_set_direction(RC522_PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(RC522_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(RC522_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ── Soft Reset ────────────────────────────────────────────────────── */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Verificar versão do chip (0x91 = v1.0, 0x92 = v2.0) */
    uint8_t version = rc522_read_reg(RC522_REG_VERSION);
    ESP_LOGI(TAG, "MFRC522 versão: 0x%02X", version);
    if (version != 0x91 && version != 0x92 && version != 0x88) {
        ESP_LOGW(TAG, "Versão inesperada — verifique as conexões SPI!");
    }

    /* ── Configuração de Timers (para timeout de comunicação) ──────────── */
    rc522_write_reg(RC522_REG_T_MODE, 0x8D);       /* TAuto=1, prescaler MSB */
    rc522_write_reg(RC522_REG_T_PRESCALER, 0x3E);   /* Prescaler LSB */
    rc522_write_reg(RC522_REG_T_RELOAD_H, 0x00);    /* Timer reload: 30 */
    rc522_write_reg(RC522_REG_T_RELOAD_L, 0x1E);
    rc522_write_reg(RC522_REG_TX_ASK, 0x40);         /* 100% ASK modulation */
    rc522_write_reg(RC522_REG_MODE, 0x3D);           /* CRC preset 0x6363 */

    /* Habilitar antena (TX1 e TX2) */
    uint8_t tx_ctrl = rc522_read_reg(RC522_REG_TX_CONTROL);
    if ((tx_ctrl & 0x03) != 0x03) {
        rc522_set_bits(RC522_REG_TX_CONTROL, 0x03);
    }

    ESP_LOGI(TAG, "MFRC522 inicializado com sucesso");
    return ESP_OK;
}

bool rc522_picc_is_present(void)
{
    /*
     * Envia REQA (Request A) — 7 bits do byte 0x26.
     * Se uma TAG responder com ATQA (2 bytes), ela está presente.
     */
    uint8_t atqa[2] = {0};
    uint8_t atqa_len = sizeof(atqa);
    uint8_t reqa = PICC_CMD_REQA;

    /* REQA usa apenas 7 bits */
    esp_err_t ret = rc522_transceive(&reqa, 1, atqa, &atqa_len, 7);
    return (ret == ESP_OK && atqa_len == 2);
}

esp_err_t rc522_picc_read_uid(rc522_uid_t *uid)
{
    if (!uid) return ESP_ERR_INVALID_ARG;
    memset(uid, 0, sizeof(rc522_uid_t));

    /*
     * Anti-Collision (Cascade Level 1):
     *   TX: [0x93, 0x20]  → NVB = 0x20 (sem UID conhecido)
     *   RX: [UID0, UID1, UID2, UID3, BCC]  → 5 bytes
     */
    uint8_t cmd[2] = {PICC_CMD_SEL_CL1, 0x20};
    uint8_t rx[5]  = {0};
    uint8_t rx_len = sizeof(rx);

    esp_err_t ret = rc522_transceive(cmd, 2, rx, &rx_len, 0);
    if (ret != ESP_OK || rx_len < 5) {
        return ESP_FAIL;
    }

    /* Verificar BCC (XOR dos 4 bytes de UID) */
    uint8_t bcc = rx[0] ^ rx[1] ^ rx[2] ^ rx[3];
    if (bcc != rx[4]) {
        ESP_LOGW(TAG, "BCC mismatch: calculado=0x%02X, recebido=0x%02X", bcc, rx[4]);
        return ESP_FAIL;
    }

    /* Copiar UID */
    memcpy(uid->uid, rx, 4);
    uid->uid_len = 4;

    ESP_LOGD(TAG, "UID lido: %02X:%02X:%02X:%02X",
             uid->uid[0], uid->uid[1], uid->uid[2], uid->uid[3]);

    return ESP_OK;
}

void rc522_picc_halt(void)
{
    /*
     * HALT: Envia [0x50, 0x00] + CRC_A.
     * A TAG não responde ao HALT — ignoramos timeout.
     */
    uint8_t cmd[4] = {PICC_CMD_HLTA, 0x00, 0x00, 0x00};

    /* Calcular CRC dos dois primeiros bytes */
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_IDLE);
    rc522_write_reg(RC522_REG_COM_IRQ, 0x7F);
    rc522_set_bits(RC522_REG_FIFO_LEVEL, 0x80);
    rc522_write_reg(RC522_REG_FIFO_DATA, cmd[0]);
    rc522_write_reg(RC522_REG_FIFO_DATA, cmd[1]);
    rc522_write_reg(RC522_REG_COMMAND, RC522_CMD_CALC_CRC);

    uint16_t wait = 5000;
    while (wait-- > 0) {
        uint8_t irq = rc522_read_reg(RC522_REG_DIV_IRQ);
        if (irq & 0x04) break;  /* CRCIRq */
    }

    cmd[2] = rc522_read_reg(RC522_REG_CRC_RESULT_L);
    cmd[3] = rc522_read_reg(RC522_REG_CRC_RESULT_H);

    uint8_t rx_len = 0;
    rc522_transceive(cmd, 4, NULL, &rx_len, 0);  /* Ignora resposta */
}

bool rc522_uid_match(const rc522_uid_t *uid, const uint8_t *target, uint8_t len)
{
    if (!uid || !target || uid->uid_len != len) return false;
    return memcmp(uid->uid, target, len) == 0;
}
