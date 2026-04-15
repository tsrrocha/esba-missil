/**
 * @file main.c
 * @brief Sistema Tático Multimissão ESP32 — Dominação + Ignitor RFID
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  ARQUITETURA FreeRTOS — 3 Tasks em 2 Cores
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  CORE 0                          │  CORE 1                         │
 *  │                                  │                                 │
 *  │  Task_Security (Prioridade 4)    │  Task_GameLogic (Prioridade 3)  │
 *  │  - Polling RFID MFRC522         │  - Monitorar botões (debounce)  │
 *  │  - Detecção de TAG autorizada   │  - Hold 5s para captura         │
 *  │  - Acionamento do relé 3s       │  - Cronômetros de posse         │
 *  │  - Comunicação via spinlock     │                                 │
 *  │                                  │  Task_UI (Prioridade 2)         │
 *  │                                  │  - Atualização LCD a cada 500ms │
 *  │                                  │  - Exibição de status e tempos  │
 *  └─────────────────────────────────────────────────────────────────────┘
 *
 *  JUSTIFICATIVA DAS PRIORIDADES:
 *  ─────────────────────────────────────────────────────────────────────────
 *  • Task_Security (P4 — Máxima): A segurança do sistema de ignição é
 *    crítica. O polling RFID deve ter garantia de execução para não perder
 *    leituras de TAG. Roda isolada no Core 0 para não competir com UI/Game.
 *
 *  • Task_GameLogic (P3 — Alta): A lógica do jogo requer timing preciso
 *    para debounce e medição de 5 segundos de pressionamento. Prioridade
 *    maior que a UI garante que a contagem não sofra jitter por atualizações
 *    de display que são operações I2C lentas (~1ms por caractere).
 *
 *  • Task_UI (P2 — Normal): A atualização do LCD é tolerante a jitter
 *    de até 100ms sem impacto perceptível ao usuário. Portanto recebe a
 *    menor prioridade, cedendo CPU para a lógica do jogo quando necessário.
 *
 *  COMUNICAÇÃO INTER-TASK:
 *  ─────────────────────────────────────────────────────────────────────────
 *  Utiliza-se um spinlock (portMUX_TYPE) para proteger a estrutura
 *  game_state_t compartilhada. O spinlock é preferido a um mutex/semáforo
 *  porque as seções críticas são extremamente curtas (cópias de structs),
 *  e o spinlock não causa inversão de prioridade nem requer context switch.
 *
 *  NOTA SOBRE PINAGEM:
 *  ─────────────────────────────────────────────────────────────────────────
 *  O pino RST do MFRC522 foi mapeado para GPIO 4 (e não GPIO 22 como no
 *  prompt original) para evitar conflito com o SCL do I2C que tipicamente
 *  usa GPIO 22. Se o hardware conectar RST ao GPIO 22, altere RC522_PIN_RST
 *  no rfid_rc522.h e use GPIO 25 para SCL do LCD (LCD_I2C_SCL_PIN).
 *  Alternativamente, se I2C e SPI não compartilharem pinos, mantenha a
 *  configuração conforme está.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "lcd_i2c.h"
#include "rfid_rc522.h"
#include "game_logic.h"

static const char *TAG = "MAIN";

/* ────────────────────────────────────────────────────────────────────────────
 *  UID da TAG autorizada para disparo do míssil
 *
 *  Altere estes valores para corresponder à sua TAG RFID.
 *  Para descobrir o UID da sua TAG, observe o log serial na primeira
 *  leitura — o UID é impresso em nível DEBUG.
 * ──────────────────────────────────────────────────────────────────────────── */
static const uint8_t AUTHORIZED_UID[] = {0x12, 0x34, 0x56, 0x78};
#define AUTHORIZED_UID_LEN  sizeof(AUTHORIZED_UID)

/* ────────────────────────────────────────────────────────────────────────────
 *  Caracteres customizados para o LCD
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Formata segundos em string "MM:SS".
 * @param sec  Total de segundos.
 * @param buf  Buffer de pelo menos 6 bytes.
 */
static void format_time(uint32_t sec, char *buf, size_t buf_size)
{
    uint32_t m = sec / 60;
    uint32_t s = sec % 60;
    snprintf(buf, buf_size, "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Task_UI — Atualização do LCD (Core 1, Prioridade 2)
 * ════════════════════════════════════════════════════════════════════════════
 *
 *  Layout do LCD 16x4:
 *  ┌────────────────┐
 *  │ DOMINACAO GAME │  Linha 0: Título
 *  │ AMA:05:23 AZU: │  Linha 1: Tempo Amarelo / Azul
 *  │ 12:45  DOM:AMA │  Linha 1 cont. / Linha 2: Status
 *  │ >>> MISSILE! <<│  Linha 3: Evento especial
 *  └────────────────┘
 *
 *  Otimização: Cada campo é atualizado individualmente com lcd_print_at()
 *  para minimizar o tempo de I2C na seção de atualização. Isto mantém o
 *  tempo total de atualização do LCD em ~15-20ms, permitindo que a
 *  Task_GameLogic execute sem starvation.
 */
static void Task_UI(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Task_UI iniciada no Core %d", xPortGetCoreID());

    /* Tela de boot */
    lcd_clear();
    lcd_print_at(0, 0, " SISTEMA TATICO ");
    lcd_print_at(0, 1, "  MULTIMISSAO   ");
    lcd_print_at(0, 2, "  ESP32  v1.0   ");
    lcd_print_at(0, 3, "   LOADING...   ");
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Cabeçalhos fixos */
    lcd_clear();

    char line_buf[32];  /* Buffer de formatação */
    game_state_t state;
    TickType_t last_wake = xTaskGetTickCount();

    /* Controle do display de "MISSILE FIRED" */
    uint32_t missile_display_timer = 0;
    bool showing_missile = false;

    for (;;) {
        game_get_state(&state);

        /* ── Linha 0: Título + Status de domínio ──────────────────────── */
        const char *dom_str;
        switch (state.dominator) {
            case TEAM_YELLOW: dom_str = "  DOM: AMARELO  "; break;
            case TEAM_BLUE:   dom_str = "   DOM: AZUL    "; break;
            default:          dom_str = " SEM DOMINACAO  "; break;
        }
        lcd_print_at(0, 0, dom_str);

        /* ── Linha 1: Cronômetros ─────────────────────────────────────── */
        char t_yellow[8], t_blue[8];
        format_time(state.yellow_time_sec, t_yellow, sizeof(t_yellow));
        format_time(state.blue_time_sec, t_blue, sizeof(t_blue));
        lcd_set_cursor(0, 1);
        lcd_print("A:");
        lcd_print(t_yellow);
        lcd_print("  B:");
        lcd_print(t_blue);
        lcd_print("  ");  /* Limpa resíduos */

        /* ── Linha 2: Progresso de captura / info ─────────────────────── */
        if (state.capturing) {
            uint32_t pct = (state.capture_progress_ms * 100) / CAPTURE_HOLD_MS;
            if (pct > 100) pct = 100;
            const char *team_name = (state.capturing_team == TEAM_YELLOW)
                                    ? "AMA" : "AZU";
            snprintf(line_buf, sizeof(line_buf), "CAP %s: %3lu%%   ", team_name,
                     (unsigned long)pct);
            lcd_print_at(0, 2, line_buf);
        } else {
            /* Mostrar indicador do time dominante com seta */
            switch (state.dominator) {
                case TEAM_YELLOW:
                    lcd_print_at(0, 2, ">>> AMARELO <<< ");
                    break;
                case TEAM_BLUE:
                    lcd_print_at(0, 2, ">>>  AZUL   <<< ");
                    break;
                default:
                    lcd_print_at(0, 2, "  PRESS 5s CAP  ");
                    break;
            }
        }

        /* ── Linha 3: Status do míssil / mensagem ─────────────────────── */
        if (state.missile_fired && !showing_missile) {
            showing_missile = true;
            missile_display_timer = 0;
        }

        if (showing_missile) {
            /* Piscar a mensagem "MISSILE FIRED!" */
            missile_display_timer += 500;
            if ((missile_display_timer / 500) % 2) {
                lcd_print_at(0, 3, "MISSILE FIRED!!!");
            } else {
                lcd_print_at(0, 3, "                ");
            }

            /* Mostrar por 5 segundos */
            if (missile_display_timer >= 5000) {
                showing_missile = false;
                game_set_missile_fired(false);
                lcd_print_at(0, 3, "  RFID PRONTO   ");
            }
        } else {
            lcd_print_at(0, 3, "  RFID PRONTO   ");
        }

        /* ── Aguardar 500ms (período de atualização do LCD) ───────────── */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(500));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Task_GameLogic — Lógica de Dominação (Core 1, Prioridade 3)
 * ════════════════════════════════════════════════════════════════════════════
 *
 *  Executa a cada 20ms para resolução adequada de debounce (50ms) e
 *  medição precisa do hold de 5000ms. O período de 20ms oferece:
 *    - Margem de 2.5x para o debounce de 50ms
 *    - Erro máximo de ±20ms na medição de 5000ms (~0.4%)
 *    - CPU usage mínimo (~0.1% do Core 1)
 *
 *  A função game_tick() é completamente não-bloqueante: ela lê GPIOs,
 *  atualiza timers via delta_ms e modifica o estado compartilhado
 *  dentro de seções críticas ultra-curtas.
 */
static void Task_GameLogic(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Task_GameLogic iniciada no Core %d", xPortGetCoreID());

    const uint32_t tick_period_ms = 20;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        game_tick(tick_period_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(tick_period_ms));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Task_Security — Polling RFID e Ignição (Core 0, Prioridade 4)
 * ════════════════════════════════════════════════════════════════════════════
 *
 *  Roda no Core 0 isolada das demais tasks para garantir:
 *    1. O timing SPI do MFRC522 não seja interrompido por I2C do LCD
 *    2. A latência de detecção de TAG seja mínima (~100ms por ciclo)
 *    3. O acionamento do relé tenha prioridade máxima
 *
 *  Fluxo de detecção:
 *    1. rc522_picc_is_present() → envia REQA, busca ATQA
 *    2. Se TAG presente → rc522_picc_read_uid() → anti-collision
 *    3. Compara UID com AUTHORIZED_UID
 *    4. Se autorizada → aciona relé por 3s + sinaliza à Task_UI
 *    5. rc522_picc_halt() → encerra sessão com a TAG
 *
 *  SEGURANÇA: Após disparo, um cooldown de 5 segundos é aplicado para
 *  evitar acionamentos acidentais em sequência.
 */
static void Task_Security(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Task_Security iniciada no Core %d", xPortGetCoreID());

    /* Cooldown após disparo para evitar acionamento repetido */
    bool in_cooldown = false;
    uint32_t cooldown_ms = 0;
    const uint32_t COOLDOWN_PERIOD_MS = 5000;
    const uint32_t POLL_PERIOD_MS = 100;

    for (;;) {
        /* ── Gerenciar cooldown ────────────────────────────────────────── */
        if (in_cooldown) {
            cooldown_ms += POLL_PERIOD_MS;
            if (cooldown_ms >= COOLDOWN_PERIOD_MS) {
                in_cooldown = false;
                cooldown_ms = 0;
                ESP_LOGI(TAG, "Cooldown encerrado — RFID ativo");
            }
            vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
            continue;
        }

        /* ── Polling RFID ──────────────────────────────────────────────── */
        if (rc522_picc_is_present()) {
            rc522_uid_t uid;
            if (rc522_picc_read_uid(&uid) == ESP_OK) {
                ESP_LOGI(TAG, "TAG detectada — UID: %02X:%02X:%02X:%02X",
                         uid.uid[0], uid.uid[1], uid.uid[2], uid.uid[3]);

                /* Verificar se é a TAG autorizada */
                if (rc522_uid_match(&uid, AUTHORIZED_UID, AUTHORIZED_UID_LEN)) {
                    ESP_LOGW(TAG, "************************************");
                    ESP_LOGW(TAG, "*  TAG AUTORIZADA — DISPARANDO!    *");
                    ESP_LOGW(TAG, "************************************");

                    /* Sinalizar à Task_UI */
                    game_set_missile_fired(true);

                    /* Feedback sonoro: 3 beeps rápidos */
                    game_buzzer_beep(100);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    game_buzzer_beep(100);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    game_buzzer_beep(100);

                    /* Acionar relé de ignição por 3 segundos */
                    game_fire_relay();

                    /* Iniciar cooldown */
                    in_cooldown = true;
                    cooldown_ms = 0;
                } else {
                    ESP_LOGW(TAG, "TAG não autorizada — ignorando");
                }

                /* Encerrar comunicação com a TAG */
                rc522_picc_halt();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  app_main() — Ponto de entrada do sistema
 * ════════════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  SISTEMA TÁTICO MULTIMISSÃO — ESP32 v1.0");
    ESP_LOGI(TAG, "  Dominação + Ignitor RFID");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    /* ── Inicialização de periféricos ──────────────────────────────────── */
    ESP_LOGI(TAG, "[1/3] Inicializando LCD I2C...");
    ESP_ERROR_CHECK(lcd_init());

    ESP_LOGI(TAG, "[2/3] Inicializando GPIOs do jogo...");
    game_gpio_init();

    ESP_LOGI(TAG, "[3/3] Inicializando RFID MFRC522...");
    ESP_ERROR_CHECK(rc522_init());

    /* ── Criação das Tasks FreeRTOS ────────────────────────────────────── */
    /*
     * xTaskCreatePinnedToCore() permite fixar cada task em um core específico.
     *
     * Stack sizes:
     *   Task_UI:        4096 bytes — buffer de formatação + chamadas I2C
     *   Task_GameLogic: 2048 bytes — lógica simples, sem alocação dinâmica
     *   Task_Security:  4096 bytes — buffers SPI + processamento RFID
     */

    ESP_LOGI(TAG, "Criando tasks FreeRTOS...");

    /* Task_UI: Core 1, Prioridade 2 (menor — tolerante a jitter) */
    BaseType_t ret;
    ret = xTaskCreatePinnedToCore(
        Task_UI,            /* Função da task                               */
        "Task_UI",          /* Nome para debug                              */
        4096,               /* Stack size em bytes                          */
        NULL,               /* Parâmetro passado à task                     */
        2,                  /* Prioridade: 2 (mais baixa das 3 tasks)       */
        NULL,               /* Handle (não necessário)                      */
        1                   /* Core 1: compartilhado com GameLogic          */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar Task_UI!");
    }

    /* Task_GameLogic: Core 1, Prioridade 3 (alta — timing preciso) */
    ret = xTaskCreatePinnedToCore(
        Task_GameLogic,
        "Task_GameLogic",
        2048,
        NULL,
        3,                  /* Prioridade: 3 (preempts Task_UI no Core 1)   */
        NULL,
        1                   /* Core 1: preempção garante timing dos botões  */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar Task_GameLogic!");
    }

    /* Task_Security: Core 0, Prioridade 4 (máxima — ignição crítica) */
    ret = xTaskCreatePinnedToCore(
        Task_Security,
        "Task_Security",
        4096,
        NULL,
        4,                  /* Prioridade: 4 (máxima — segurança do míssil) */
        NULL,
        0                   /* Core 0: isolada para SPI exclusivo           */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar Task_Security!");
    }

    ESP_LOGI(TAG, "Sistema ativo — aguardando interação...");

    /* app_main() retorna — FreeRTOS scheduler já está ativo no ESP-IDF */
}
