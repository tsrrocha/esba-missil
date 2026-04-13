#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// O LED interno na maioria das placas ESP32 DevKit está no GPIO 2.
// Altere se a sua placa usar um pino diferente.
#define BLINK_GPIO 2

static const char *TAG = "BLINK_TEST";

void app_main(void)
{
    /* Configuração do pino do LED */
    ESP_LOGI(TAG, "Configurando GPIO %d como saída...", BLINK_GPIO);
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        /* Liga o LED */
        ESP_LOGI(TAG, "LED ON");
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        /* Desliga o LED */
        ESP_LOGI(TAG, "LED OFF");
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
