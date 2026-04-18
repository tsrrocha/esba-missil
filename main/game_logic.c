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
#include "nvs_flash.h"
#include "nvs.h"
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

/* Acumulador para salvar NVS a cada NVS_SAVE_INTERVAL_MS */
static uint32_t s_nvs_save_accumulator_ms = 0;

/* Últimos valores salvos na NVS (para evitar escritas desnecessárias) */
static uint32_t s_last_saved_yellow = 0;
static uint32_t s_last_saved_blue   = 0;
static team_t   s_last_saved_dom    = TEAM_NONE;

/* ────────────────────────────────────────────────────────────────────────────
 *  Namespace e chaves NVS
 * ──────────────────────────────────────────────────────────────────────────── */
#define NVS_NAMESPACE   "game"        /**< Namespace NVS para dados do jogo  */
#define NVS_KEY_YELLOW  "yellow_sec"  /**< Chave: tempo Amarelo em segundos  */
#define NVS_KEY_BLUE    "blue_sec"    /**< Chave: tempo Azul em segundos     */
#define NVS_KEY_DOM     "dominator"   /**< Chave: time dominante (0/1/2)     */

static nvs_handle_t s_nvs_handle = 0; /**< Handle NVS aberto                */

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
    /* Apenas mostra progresso se o time NÃO for o dominador atual */
    if (!s_btn_yellow.last_stable && s_btn_yellow.hold_timer > 0 && s_state.dominator != TEAM_YELLOW) {
        s_state.capturing      = true;
        s_state.capturing_team = TEAM_YELLOW;
        s_state.capture_progress_ms = s_btn_yellow.hold_timer;
    } else if (!s_btn_blue.last_stable && s_btn_blue.hold_timer > 0 && s_state.dominator != TEAM_BLUE) {
        s_state.capturing      = true;
        s_state.capturing_team = TEAM_BLUE;
        s_state.capture_progress_ms = s_btn_blue.hold_timer;
    } else {
        s_state.capturing      = false;
        s_state.capturing_team = TEAM_NONE;
        s_state.capture_progress_ms = 0;
    }

    /* ── Troca de domínio ──────────────────────────────────────────────── */
    if (yellow_captured) {
        portEXIT_CRITICAL(&s_state_mux);
        
        bool was_none;
        portENTER_CRITICAL(&s_state_mux);
        was_none = (s_state.dominator == TEAM_NONE);
        
        if (s_state.dominator == TEAM_BLUE) {
            s_state.dominator = TEAM_NONE;
            portEXIT_CRITICAL(&s_state_mux);
            ESP_LOGW(TAG, "!!! PONTO NEUTRALIZADO PELA EQUIPE AMARELA !!!");
            game_buzzer_beep(200);
        } else if (was_none) {
            s_state.dominator = TEAM_YELLOW;
            portEXIT_CRITICAL(&s_state_mux);
            ESP_LOGI(TAG, "*** EQUIPE AMARELA DOMINA ***");
            game_buzzer_beep(200);
        } else {
            portEXIT_CRITICAL(&s_state_mux);
        }
        return;
    }

    if (blue_captured) {
        portEXIT_CRITICAL(&s_state_mux);
        
        bool was_none;
        portENTER_CRITICAL(&s_state_mux);
        was_none = (s_state.dominator == TEAM_NONE);
        
        if (s_state.dominator == TEAM_YELLOW) {
            s_state.dominator = TEAM_NONE;
            portEXIT_CRITICAL(&s_state_mux);
            ESP_LOGW(TAG, "!!! PONTO NEUTRALIZADO PELA EQUIPE AZUL !!!");
            game_buzzer_beep(200);
        } else if (was_none) {
            s_state.dominator = TEAM_BLUE;
            portEXIT_CRITICAL(&s_state_mux);
            ESP_LOGI(TAG, "*** EQUIPE AZUL DOMINA ***");
            game_buzzer_beep(200);
        } else {
            portEXIT_CRITICAL(&s_state_mux);
        }
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

    /* ── Auto-save NVS a cada NVS_SAVE_INTERVAL_MS ─────────────────────── */
    s_nvs_save_accumulator_ms += delta_ms;
    if (s_nvs_save_accumulator_ms >= NVS_SAVE_INTERVAL_MS) {
        s_nvs_save_accumulator_ms = 0;
        game_nvs_save();
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 *  Persistência NVS (Non-Volatile Storage)
 *
 *  A NVS do ESP32 usa partição de flash com wear-leveling interno.
 *  Com salvamento a cada 60 segundos, a vida útil estimada da flash
 *  (100.000 ciclos) permite ~6.900 horas de jogo contínuo (~288 dias).
 *
 *  Fluxo:
 *    1. game_nvs_init()  → Inicializa flash + abre namespace + carrega dados
 *    2. game_nvs_save()  → Salva apenas se houve mudança (dirty check)
 *    3. game_nvs_reset() → Zera RAM + apaga chaves NVS (nova partida)
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t game_nvs_init(void)
{
    ESP_LOGI(TAG, "Inicializando NVS Flash...");

    /* Inicializar subsistema NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Partição corrompida ou versão incompatível — apagar e reinicializar */
        ESP_LOGW(TAG, "NVS corrompida/incompatível — apagando e reinicializando");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Abrir namespace para dados do jogo */
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS namespace '%s': %s",
                 NVS_NAMESPACE, esp_err_to_name(ret));
        return ret;
    }

    /* ── Tentar restaurar dados salvos ──────────────────────────────────── */
    uint32_t yellow = 0, blue = 0;
    int32_t  dom = 0;
    bool has_data = false;

    if (nvs_get_u32(s_nvs_handle, NVS_KEY_YELLOW, &yellow) == ESP_OK) {
        has_data = true;
    }
    if (nvs_get_u32(s_nvs_handle, NVS_KEY_BLUE, &blue) == ESP_OK) {
        has_data = true;
    }
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_DOM, &dom) == ESP_OK) {
        has_data = true;
    }

    if (has_data) {
        portENTER_CRITICAL(&s_state_mux);
        s_state.yellow_time_sec = yellow;
        s_state.blue_time_sec   = blue;
        s_state.dominator       = (team_t)dom;
        portEXIT_CRITICAL(&s_state_mux);

        /* Atualizar cache de comparação */
        s_last_saved_yellow = yellow;
        s_last_saved_blue   = blue;
        s_last_saved_dom    = (team_t)dom;

        ESP_LOGI(TAG, "Estado restaurado da NVS:");
        ESP_LOGI(TAG, "  Amarelo: %lu s | Azul: %lu s | Dominator: %d",
                 (unsigned long)yellow, (unsigned long)blue, (int)dom);
    } else {
        ESP_LOGI(TAG, "Nenhum dado salvo encontrado — nova partida");
    }

    return ESP_OK;
}

void game_nvs_save(void)
{
    if (s_nvs_handle == 0) return;  /* NVS não inicializada */

    /* Ler estado atual de forma thread-safe */
    game_state_t snap;
    game_get_state(&snap);

    /* Dirty check: só escrever na flash se algo mudou */
    bool dirty = false;
    if (snap.yellow_time_sec != s_last_saved_yellow) dirty = true;
    if (snap.blue_time_sec   != s_last_saved_blue)   dirty = true;
    if (snap.dominator       != s_last_saved_dom)    dirty = true;

    if (!dirty) {
        ESP_LOGD(TAG, "NVS: sem alterações — save ignorado");
        return;
    }

    /* Escrever valores atualizados */
    esp_err_t ret;
    ret = nvs_set_u32(s_nvs_handle, NVS_KEY_YELLOW, snap.yellow_time_sec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS: falha ao salvar yellow_sec: %s", esp_err_to_name(ret));
    }

    ret = nvs_set_u32(s_nvs_handle, NVS_KEY_BLUE, snap.blue_time_sec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS: falha ao salvar blue_sec: %s", esp_err_to_name(ret));
    }

    ret = nvs_set_i32(s_nvs_handle, NVS_KEY_DOM, (int32_t)snap.dominator);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS: falha ao salvar dominator: %s", esp_err_to_name(ret));
    }

    /* Commit (flush para flash) */
    ret = nvs_commit(s_nvs_handle);
    if (ret == ESP_OK) {
        s_last_saved_yellow = snap.yellow_time_sec;
        s_last_saved_blue   = snap.blue_time_sec;
        s_last_saved_dom    = snap.dominator;
        ESP_LOGI(TAG, "NVS salvo: AMA=%lu s | AZU=%lu s | DOM=%d",
                 (unsigned long)snap.yellow_time_sec,
                 (unsigned long)snap.blue_time_sec,
                 (int)snap.dominator);
    } else {
        ESP_LOGE(TAG, "NVS: falha no commit: %s", esp_err_to_name(ret));
    }
}

void game_nvs_reset(void)
{
    ESP_LOGW(TAG, "Resetando dados da partida (RAM + NVS)...");

    /* Zerar estado na RAM */
    portENTER_CRITICAL(&s_state_mux);
    s_state.yellow_time_sec = 0;
    s_state.blue_time_sec   = 0;
    s_state.dominator       = TEAM_NONE;
    portEXIT_CRITICAL(&s_state_mux);

    s_possession_accumulator_ms = 0;
    s_nvs_save_accumulator_ms   = 0;
    s_last_saved_yellow = 0;
    s_last_saved_blue   = 0;
    s_last_saved_dom    = TEAM_NONE;

    /* Apagar chaves na NVS */
    if (s_nvs_handle != 0) {
        nvs_erase_key(s_nvs_handle, NVS_KEY_YELLOW);
        nvs_erase_key(s_nvs_handle, NVS_KEY_BLUE);
        nvs_erase_key(s_nvs_handle, NVS_KEY_DOM);
        nvs_commit(s_nvs_handle);
    }

    ESP_LOGI(TAG, "Dados resetados — pronto para nova partida");
}
