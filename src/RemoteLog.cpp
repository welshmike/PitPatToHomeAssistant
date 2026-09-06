#include "RemoteLog.h"

#ifdef ARDUINO

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "esp_system.h"

namespace
{
struct Line
{
    char text[RemoteLog::kLineLen];
};

// 24 x 120 B = 2.8 KB of internal RAM, allocated once in begin(). Deep enough
// to hold the whole boot-time BLE story (connect, subscribe, first kick) until
// WiFi and MQTT come up ~5 s later, shallow enough not to matter next to the
// net task's 12 KB stack.
constexpr UBaseType_t kDepth = 24;

// Formatting scratch, on the stack of whichever task logged. Bigger than
// kLineLen so a long line is truncated once, on copy, rather than twice.
constexpr size_t kFormatBuf = 240;

QueueHandle_t s_queue = nullptr;

// The vprintf esp_log used before us. Seeded with vprintf so the hook can never
// see a null even if another task logs inside esp_log_set_vprintf().
vprintf_like_t s_prev = &vprintf;

// Lines the filter wanted but the queue had no room for; folded into the next
// line that does get through. Touched from any task, so atomic — 32-bit is
// lock-free on both boards, unlike a 16-bit counter which would go through
// libatomic.
std::atomic<uint32_t> s_dropped{0};

// I lines are only worth the airtime when they are part of the connect/kick
// story we are chasing (spec 4.14); everything else at I is the 15 s heap
// heartbeat and per-command chatter. E and W are always forwarded, so this
// list only has to cover the interesting INFO lines.
const char *const kInfoKeywords[] = {
    "connect", "Connect", "kick", "Kick", "Subscribed", "BLE", "Net status", "boot",
};

bool infoWanted(const char *line)
{
    for (const char *needle : kInfoKeywords)
    {
        if (strstr(line, needle) != nullptr)
        {
            return true;
        }
    }
    return false;
}

// ESP-IDF formats every line as "<letter> (<timestamp>) <tag>: <message>",
// optionally behind an ANSI colour escape (CONFIG_LOG_COLORS is 0 in the
// packaged sdkconfig, but a line from a library that adds its own colour must
// not be mistaken for a level-less line). Returns 'E', 'W', 'I', 'D' or 'V',
// or 'I' when there is no recognisable prefix — a bare printf-style line is
// treated as informational and goes through the keyword filter.
char levelOf(const char *line)
{
    const char *p = line;
    if (*p == '\033')
    {
        while (*p != '\0' && *p != 'm')
        {
            p++;
        }
        if (*p == 'm')
        {
            p++;
        }
    }
    if (p[0] != '\0' && p[1] == ' ' && p[2] == '(')
    {
        switch (p[0])
        {
        case 'E':
        case 'W':
        case 'I':
        case 'D':
        case 'V':
            return p[0];
        default:
            break;
        }
    }
    return 'I';
}

// Runs on whichever task logged — the loop task, the net task, the NimBLE host
// task, a WiFi event task. Allocates nothing and never blocks: the formatting
// buffer is on the caller's stack and the queue send has a zero timeout.
int hook(const char *fmt, va_list ap)
{
    // Serial first and unconditionally, so a full queue, a bad filter or a
    // truncation bug can never cost us the serial log.
    va_list copy;
    va_copy(copy, ap);
    const int written = s_prev(fmt, ap);

    if (s_queue == nullptr)
    {
        va_end(copy);
        return written;
    }

    char buf[kFormatBuf];
    const int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n <= 0)
    {
        return written;
    }

    const char level = levelOf(buf);
    if (level == 'D' || level == 'V')
    {
        return written;
    }

    // Strip the trailing "\r\n" (or "\n") so the MQTT payload is one clean line.
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    {
        buf[--len] = '\0';
    }
    if (len == 0)
    {
        return written;
    }

    if (level == 'I' && !infoWanted(buf))
    {
        return written;
    }

    Line line;
    const uint32_t dropped = s_dropped.load(std::memory_order_relaxed);
    if (dropped != 0)
    {
        snprintf(line.text, sizeof(line.text), "[+%u dropped] %s", (unsigned)dropped, buf);
    }
    else
    {
        snprintf(line.text, sizeof(line.text), "%s", buf);
    }

    BaseType_t sent;
    if (xPortInIsrContext())
    {
        // Not expected — esp_log is never called from an ISR on either board —
        // but the hook is installed process-wide, so handle it rather than
        // corrupting the queue if some driver ever does.
        BaseType_t woken = pdFALSE;
        sent = xQueueSendFromISR(s_queue, &line, &woken);
        if (woken == pdTRUE)
        {
            portYIELD_FROM_ISR();
        }
    }
    else
    {
        sent = xQueueSend(s_queue, &line, 0);
    }

    if (sent == pdTRUE)
    {
        // Only clear what this line actually reported.
        s_dropped.fetch_sub(dropped, std::memory_order_relaxed);
    }
    else
    {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return written;
}
} // namespace

namespace RemoteLog
{
void begin()
{
    if (s_queue != nullptr)
    {
        return;
    }
    s_queue = xQueueCreate(kDepth, sizeof(Line));
    if (s_queue == nullptr)
    {
        return; // no queue, no hook — serial logging carries on untouched
    }

    // Arduino's initArduino() has already run esp_log_level_set("*",
    // CONFIG_LOG_DEFAULT_LEVEL), and the packaged sdkconfig sets that to ERROR.
    // Without this, every log_w()/log_i() the firmware compiles in at
    // CORE_DEBUG_LEVEL=3 would be dropped inside esp_log_write() — on serial as
    // well as here — the moment USE_ESP_IDF_LOG routes them through ESP-IDF.
    // INFO (not VERBOSE) keeps the IDF's own debug chatter out.
    esp_log_level_set("*", ESP_LOG_INFO);

    s_prev = esp_log_set_vprintf(hook);
    if (s_prev == nullptr)
    {
        s_prev = &vprintf;
    }
}

bool pop(char *out, size_t cap)
{
    if (s_queue == nullptr || out == nullptr || cap == 0)
    {
        return false;
    }
    Line line;
    if (xQueueReceive(s_queue, &line, 0) != pdTRUE)
    {
        return false;
    }
    line.text[sizeof(line.text) - 1] = '\0';
    snprintf(out, cap, "%s", line.text);
    return true;
}

const char *resetReasonName()
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "UNKNOWN";
    }
}

const char *buildStamp()
{
    return __DATE__ " " __TIME__;
}
} // namespace RemoteLog

#endif // ARDUINO
