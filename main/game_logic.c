/**
 * @file game_logic.c
 * @brief Implementação da lógica de Dominação e controle de hardware
 *
 * Estado do jogo protegido por portMUX (spinlock) para acesso concorrente
 * entre as tasks Task_GameLogic (Core 1) e Task_Security (Core 0).
 *
 * A detecção de botão usa uma abordagem não-bloqueante:
 *   1. Debounce: GPIO deve estar estável em LOW por DEBOUNCE_MS
 *   2. Hold timer: Após debounce, acumula tempo em LOW via delta_ms
 *   3. Se o botão for solto antes de 5s, o timer reseta
 *   4. Se atingir 5s, o domínio é alternado
 */

#include "game_logic.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GAME";

/* ────────────────────────────────────────────────────────────────────────────
 *  Estado protegido por spinlock
 * ──────────────────────────────────────────────────────────────────────────── */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static game_state_t s_state = {
    .dominator         = TEAM_NONE,
    .yellow_time_sec   = 0,
    .blue_time_sec     = 0,
    .missile_fired     = false,
    .capturing         = false,
    .capturing_team    = TEAM_NONE,
    .capture_progress_ms = 0,
};

/* ────────────────────────────────────────────────────────────────────────────
 *  Estado interno de debounce (usado apenas na Task_GameLogic)
 * ──────────────────────────────────────────────────────────────────────────── */
typedef struct {
    int      gpio;
    team_t   team;
    bool     last_stable;      /**< Último estado estável do botão         */
    bool     raw_last;         /**< Última leitura crua do GPIO            */
    uint32_t debounce_timer;   /**< Acumulador de debounce em ms           */
    uint32_t hold_timer;       /**< Acumulador de hold em ms               */
} button_ctx_t;

static button_ctx_t s_btn_yellow;
static button_ctx_t s_btn_blue;

/* Acumulador sub-segundo para contagem de tempo de posse */
static uint32_t s_possession_accumulator_ms = 0;

/* ────────────────────────────────────────────────────────────────────────────
 *  Funções internas
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa o contexto de um botão.
 */
static void button_ctx_init(button_ctx_t *ctx, int gpio, team_t team)
{
    ctx->gpio           = gpio;
    ctx->team           = team;
    ctx->last_stable    = true;   /* Pull-up → HIGH em repouso */
    ctx->raw_last       = true;
    ctx->debounce_timer = 0;
    ctx->hold_timer     = 0;
}

/**
 * @brief Processa debounce e detecção de hold de um botão.
 *
 * Algoritmo:
 *   1. Lê o GPIO atual
 *   2. Se diferente da última leitura crua, reinicia o timer de debounce
 *   3. Se igual por DEBOUNCE_MS consecutivos, atualiza estado estável
 *   4. Se estável em LOW, acumula hold timer
 *   5. Se hold timer >= CAPTURE_HOLD_MS, sinaliza captura
 *
 * @param ctx      Contexto do botão.
 * @param delta_ms Tempo desde a última chamada.
 * @return true se o botão atingiu os 5 segundos de hold.
 */
static bool button_process(button_ctx_t *ctx, uint32_t delta_ms)
{
    bool raw = gpio_get_level(ctx->gpio);  /* HIGH = solto (pull-up) */

    /* Detecção de mudança → reinicia debounce */
    if (raw != ctx->raw_last) {
        ctx->debounce_timer = 0;
    }
    ctx->raw_last = raw;

    /* Acumula tempo de estabilidade */
    ctx->debounce_timer += delta_ms;

    if (ctx->debounce_timer >= DEBOUNCE_MS) {
        /* Estado estável confirmado */
        if (raw != ctx->last_stable) {
            ctx->last_stable = raw;
            if (raw) {
                /* Botão solto → reseta hold */
                ctx->hold_timer = 0;
            }
        }

        /* Se pressionado (LOW), acumula hold */
        if (!ctx->last_stable) {
            ctx->hold_timer += delta_ms;
            if (ctx->hold_timer >= CAPTURE_HOLD_MS) {
                ctx->hold_timer = 0;  /* Reseta para evitar trigger contínuo */
                return true;
            }
        }
    }

    return false;
}

/* ────────────────────────────────────────────────────────────────────────────
 *  API Pública
 * ──────────────────────────────────────────────────────────────────────────── */

void game_gpio_init(void)
{
    ESP_LOGI(TAG, "Configurando GPIOs do jogo...");

    /* ── Botões com Pull-up interno ────────────────────────────────────── */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_YELLOW_GPIO) | (1ULL << BTN_BLUE_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* ── Relé de ignição (inicialmente desligado) ──────────────────────── */
    gpio_config_t relay_cfg = {
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&relay_cfg);
    gpio_set_level(RELAY_GPIO, 0);

    /* ── Buzzer (inicialmente desligado) ───────────────────────────────── */
    gpio_config_t buzzer_cfg = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&buzzer_cfg);
    gpio_set_level(BUZZER_GPIO, 0);

    /* ── Inicializar contextos de botões ───────────────────────────────── */
    button_ctx_init(&s_btn_yellow, BTN_YELLOW_GPIO, TEAM_YELLOW);
    button_ctx_init(&s_btn_blue,   BTN_BLUE_GPIO,   TEAM_BLUE);

    ESP_LOGI(TAG, "GPIOs inicializados: BTN_Y=%d, BTN_B=%d, RELAY=%d, BUZZER=%d",
             BTN_YELLOW_GPIO, BTN_BLUE_GPIO, RELAY_GPIO, BUZZER_GPIO);
}

void game_get_state(game_state_t *state)
{
    portENTER_CRITICAL(&s_state_mux);
    memcpy(state, &s_state, sizeof(game_state_t));
    portEXIT_CRITICAL(&s_state_mux);
}

void game_set_missile_fired(bool fired)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.missile_fired = fired;
    portEXIT_CRITICAL(&s_state_mux);
}

void game_fire_relay(void)
{
    ESP_LOGW(TAG, ">>> RELÉ ACIONADO — MÍSSIL DISPARADO! <<<");
    gpio_set_level(RELAY_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(RELAY_FIRE_MS));
    gpio_set_level(RELAY_GPIO, 0);
    ESP_LOGI(TAG, "Relé desligado");
}

void game_buzzer_beep(uint32_t duration_ms)
{
    gpio_set_level(BUZZER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(BUZZER_GPIO, 0);
}

void game_tick(uint32_t delta_ms)
{
    /* ── Processar botões ──────────────────────────────────────────────── */
    bool yellow_captured = button_process(&s_btn_yellow, delta_ms);
    bool blue_captured   = button_process(&s_btn_blue, delta_ms);

    /* ── Atualizar captura visual (progresso) ─────────────────────────── */
    portENTER_CRITICAL(&s_state_mux);

    /* Determinar se algum botão está sendo segurado (para UI de progresso) */
    if (!s_btn_yellow.last_stable && s_btn_yellow.hold_timer > 0) {
        s_state.capturing      = true;
        s_state.capturing_team = TEAM_YELLOW;
        s_state.capture_progress_ms = s_btn_yellow.hold_timer;
    } else if (!s_btn_blue.last_stable && s_btn_blue.hold_timer > 0) {
        s_state.capturing      = true;
        s_state.capturing_team = TEAM_BLUE;
        s_state.capture_progress_ms = s_btn_blue.hold_timer;
    } else {
        s_state.capturing      = false;
        s_state.capturing_team = TEAM_NONE;
        s_state.capture_progress_ms = 0;
    }

    /* ── Troca de domínio ──────────────────────────────────────────────── */
    if (yellow_captured && s_state.dominator != TEAM_YELLOW) {
        s_state.dominator = TEAM_YELLOW;
        ESP_LOGI(TAG, "*** EQUIPE AMARELA DOMINA ***");
        portEXIT_CRITICAL(&s_state_mux);
        game_buzzer_beep(200);  /* Feedback sonoro */
        return;
    }

    if (blue_captured && s_state.dominator != TEAM_BLUE) {
        s_state.dominator = TEAM_BLUE;
        ESP_LOGI(TAG, "*** EQUIPE AZUL DOMINA ***");
        portEXIT_CRITICAL(&s_state_mux);
        game_buzzer_beep(200);
        return;
    }

    /* ── Acumular tempo de posse (resolução de 1 segundo) ──────────────── */
    if (s_state.dominator != TEAM_NONE) {
        s_possession_accumulator_ms += delta_ms;
        if (s_possession_accumulator_ms >= 1000) {
            s_possession_accumulator_ms -= 1000;
            if (s_state.dominator == TEAM_YELLOW) {
                s_state.yellow_time_sec++;
            } else {
                s_state.blue_time_sec++;
            }
        }
    }

    portEXIT_CRITICAL(&s_state_mux);
}
