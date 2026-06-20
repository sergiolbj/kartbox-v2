/*
 * app_events.c
 * Ver app_events.h pro contexto - isso substitui os dois caminhos
 * separados de botao (fisico em main.c, touch em ui_kartbox.c) que a
 * v1 tinha.
 */
#include "app_events.h"
#include "config.h"

#include <string.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "app_events";

QueueHandle_t g_app_event_queue = NULL;

/* Estado de debounce por botao. stable_active = nivel ja debounced;
 * raw_active_prev = ultima leitura crua (pra detectar mudanca antes do
 * debounce confirmar); has_hold = so o RESET usa a logica de segurar. */
static struct {
    gpio_num_t pin;
    app_event_type_t press_event;
    bool     stable_active;
    bool     raw_active_prev;
    int64_t  last_change_us;
    int64_t  press_start_us;
    bool     hold_fired;
    bool     has_hold;
} s_buttons[] = {
    { .pin = BTN_MODE_PIN,    .press_event = APP_EVT_BTN_MODE,    .has_hold = false },
    { .pin = BTN_SETLINE_PIN, .press_event = APP_EVT_BTN_SETLINE, .has_hold = false },
    { .pin = BTN_RESET_PIN,   .press_event = APP_EVT_BTN_RESET,   .has_hold = true  },
};
#define NUM_BUTTONS (sizeof(s_buttons) / sizeof(s_buttons[0]))

static void button_poll_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(10); /* poll fino, debounce e que decide */

    for (;;) {
        int64_t now_us = esp_timer_get_time();

        for (size_t i = 0; i < NUM_BUTTONS; i++) {
            __typeof__(s_buttons[0]) *b = &s_buttons[i];

            /* Wiring assumido: pull-up + ativo em LOW. Inverte aqui se a
             * bancada mostrar o contrario - unico lugar que precisa mudar. */
            bool raw_active = (gpio_get_level(b->pin) == 0);

            if (raw_active != b->raw_active_prev) {
                b->last_change_us = now_us;
                b->raw_active_prev = raw_active;
            }

            bool debounced_ok = (now_us - b->last_change_us) >= (BTN_DEBOUNCE_MS * 1000);

            if (debounced_ok && raw_active != b->stable_active) {
                b->stable_active = raw_active;

                if (b->stable_active) {
                    /* borda de subida (pressionou) */
                    app_event_post(b->press_event, EVT_SRC_GPIO);
                    b->press_start_us = now_us;
                    b->hold_fired = false;
                } else {
                    /* soltou antes do hold completar - avisa UI pra cancelar barra */
                    if (b->has_hold && !b->hold_fired) {
                        app_event_post(APP_EVT_BTN_RESET_HOLD_ABORT, EVT_SRC_GPIO);
                    }
                    b->hold_fired = false;
                }
            }

            if (b->has_hold && b->stable_active && !b->hold_fired) {
                int64_t held_ms = (now_us - b->press_start_us) / 1000;
                if (held_ms >= BTN_RESET_HOLD_MS) {
                    app_event_post(APP_EVT_BTN_RESET_HELD, EVT_SRC_GPIO);
                    b->hold_fired = true;
                }
            }
        }

        vTaskDelay(period);
    }
}

void app_events_init(void)
{
    g_app_event_queue = xQueueCreate(APP_EVENT_QUEUE_LEN, sizeof(app_event_t));
    if (!g_app_event_queue) {
        ESP_LOGE(TAG, "Falha ao criar fila de eventos - sem memoria");
        abort();
    }

    const gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BTN_MODE_PIN) | (1ULL << BTN_SETLINE_PIN) | (1ULL << BTN_RESET_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE, /* polling, nao interrupcao - simples e robusto o suficiente pra 3 botoes */
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        s_buttons[i].last_change_us = now;
        s_buttons[i].raw_active_prev = (gpio_get_level(s_buttons[i].pin) == 0);
        s_buttons[i].stable_active = s_buttons[i].raw_active_prev;
    }

    xTaskCreate(button_poll_task, "btn_poll", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "Fila de eventos pronta, %d botoes fisicos sob polling", (int)NUM_BUTTONS);
}

bool app_event_post(app_event_type_t type, app_event_source_t source)
{
    app_event_t evt = { .type = type, .source = source };
    return app_event_post_data(&evt);
}

bool app_event_post_data(const app_event_t *evt)
{
    if (!g_app_event_queue) {
        ESP_LOGW(TAG, "Tentativa de postar evento antes de app_events_init()");
        return false;
    }
    if (xQueueSend(g_app_event_queue, evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Fila de eventos cheia, evento tipo %d perdido", evt->type);
        return false;
    }
    return true;
}
