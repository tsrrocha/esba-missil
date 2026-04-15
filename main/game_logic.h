/**
 * @file game_logic.h
 * @brief Lógica do modo Dominação — cronômetros de posse e detecção de botões
 *
 * Gerencia o estado do jogo de Dominação com dois times (Amarelo e Azul).
 * Cada time acumula tempo de posse enquanto controla o ponto.
 * A troca de domínio requer pressionamento sustentado de 5 segundos.
 */

#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 *  GPIOs dos Botões e Saídas
 * ──────────────────────────────────────────────────────────────────────────── */
#define BTN_YELLOW_GPIO     13      /**< Botão Equipe Amarela (pull-up)      */
#define BTN_BLUE_GPIO       14      /**< Botão Equipe Azul   (pull-up)       */
#define RELAY_GPIO          27      /**< Relé de ignição do míssil           */
#define BUZZER_GPIO         26      /**< Buzzer para feedback sonoro         */

/* ────────────────────────────────────────────────────────────────────────────
 *  Constantes de Jogo
 * ──────────────────────────────────────────────────────────────────────────── */
#define CAPTURE_HOLD_MS     5000    /**< Tempo de pressionamento para captura */
#define DEBOUNCE_MS         50      /**< Debounce dos botões                  */
#define RELAY_FIRE_MS       3000    /**< Tempo de acionamento do relé         */

/* ────────────────────────────────────────────────────────────────────────────
 *  Estados do Jogo
 * ──────────────────────────────────────────────────────────────────────────── */
typedef enum {
    TEAM_NONE   = 0,    /**< Nenhum time domina                              */
    TEAM_YELLOW = 1,    /**< Equipe Amarela domina                           */
    TEAM_BLUE   = 2,    /**< Equipe Azul domina                              */
} team_t;

/**
 * @brief Estado completo do jogo, compartilhado entre tasks.
 *
 * Acesso atômico garantido pelo uso de portENTER_CRITICAL / portEXIT_CRITICAL
 * com um spinlock dedicado.
 */
typedef struct {
    team_t   dominator;           /**< Time que atualmente domina            */
    uint32_t yellow_time_sec;     /**< Tempo de posse acumulado (Amarelo)    */
    uint32_t blue_time_sec;       /**< Tempo de posse acumulado (Azul)       */
    bool     missile_fired;       /**< Flag: míssil disparado (atômico)      */
    bool     capturing;           /**< Captura em andamento?                 */
    team_t   capturing_team;      /**< Time que está tentando capturar       */
    uint32_t capture_progress_ms; /**< Progresso da captura em ms            */
} game_state_t;

/* ────────────────────────────────────────────────────────────────────────────
 *  API Pública
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Inicializa GPIOs dos botões, relé e buzzer.
 */
void game_gpio_init(void);

/**
 * @brief Obtém uma cópia thread-safe do estado do jogo.
 * @param[out] state Cópia do estado atual.
 */
void game_get_state(game_state_t *state);

/**
 * @brief Sinaliza que o míssil foi disparado (chamado pela Task_Security).
 */
void game_set_missile_fired(bool fired);

/**
 * @brief Aciona o relé de ignição por RELAY_FIRE_MS.
 *        Deve ser chamado de dentro de uma task (usa vTaskDelay).
 */
void game_fire_relay(void);

/**
 * @brief Emite um beep curto no buzzer.
 * @param duration_ms Duração do beep em milissegundos.
 */
void game_buzzer_beep(uint32_t duration_ms);

/**
 * @brief Lógica principal do jogo — chamada ciclicamente pela Task_GameLogic.
 *
 * Monitora botões com debounce, verifica pressionamento sustentado de 5s,
 * e acumula tempo de posse do time dominante.
 *
 * @param delta_ms Tempo decorrido desde a última chamada (em ms).
 */
void game_tick(uint32_t delta_ms);

#ifdef __cplusplus
}
#endif

#endif /* GAME_LOGIC_H */
