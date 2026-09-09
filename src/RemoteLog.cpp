#include "RemoteLog.h"

#ifdef ARDUINO

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

namespace
{
struct Line
{
    char text[RemoteLog::kLineLen];
};

// ---------------------------------------------------------------------------
// Crash forensics: the last forwarded lines, kept across a reset (spec 4.16).
//
// RTC_NOINIT_ATTR puts this in RTC slow memory and, crucially, keeps the
// startup code from zeroing it: the contents survive a panic, either watchdog
// and a brownout — everything except a power cycle, where the bits are simply
// whatever the SRAM powered up as. Hence the magic word and the checksum: they
// are the only way to tell "the previous firmware wrote this" from "this is
// noise". Nothing here allocates and nothing blocks, because the writer is the
// log hook, which runs on whichever task logged.
// ---------------------------------------------------------------------------
constexpr uint32_t kRingMagic = 0x50414345; // "PACE"

struct RtcRing
{
    uint32_t magic;
    uint32_t next;  // slot the next line goes into
    uint32_t count; // valid slots, saturating at kLastLines
    // The same bytes twice: lines[][] to write and read, words[] to checksum a
    // word at a time. A union rather than a cast because casting a char array
    // to uint32_t* is exactly the aliasing GCC is allowed to reorder around.
    union
    {
        char     lines[RemoteLog::kLastLines][RemoteLog::kLineLen];
        uint32_t words[RemoteLog::kLastLines * RemoteLog::kLineLen / sizeof(uint32_t)];
    };
    uint32_t checksum;
};

RTC_NOINIT_ATTR RtcRing s_ring;

// The ring is written from any task (loop, net, NimBLE host) and read/cleared
// from the net task, so it needs mutual exclusion — but the writer is a log
// hook that must never block, which rules out a mutex. A spinlock it is: the
// critical section is a bounded copy plus the checksum, a few microseconds.
portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

// What begin() found in the ring, before this boot's own lines started
// overwriting it. Without this copy the tail of the crash would be gone by the
// time MQTT comes up ~5 s later — boot alone emits more than kLastLines
// forwarded lines. 960 B of .bss for the one thing this feature exists to show.
char    s_saved[RemoteLog::kLastLines][RemoteLog::kLineLen];
uint8_t s_savedCount = 0;

// Sum of the ring's mutable state. Word-wise rather than byte-wise only for
// speed — it runs inside the spinlock on every forwarded line. It is a
// corruption check, not a security measure; a plain sum is enough to reject the
// power-on garbage that would otherwise be published as log lines.
uint32_t ringChecksum(const RtcRing &r)
{
    static_assert(sizeof(r.lines) == sizeof(r.words), "union halves must match");
    uint32_t sum = r.next + r.count;
    for (size_t i = 0; i < sizeof(r.words) / sizeof(r.words[0]); i++)
    {
        sum += r.words[i];
    }
    return sum;
}

// Caller holds the spinlock, or is begin() before the hook is installed.
bool ringValid()
{
    return s_ring.magic == kRingMagic && s_ring.next < RemoteLog::kLastLines &&
           s_ring.count <= RemoteLog::kLastLines && s_ring.checksum == ringChecksum(s_ring);
}

// Caller holds the spinlock, or is begin() before the hook is installed.
void ringReset()
{
    s_ring.magic = kRingMagic;
    s_ring.next  = 0;
    s_ring.count = 0;
    memset(s_ring.lines, 0, sizeof(s_ring.lines));
    s_ring.checksum = ringChecksum(s_ring);
}

// Called from the log hook, on whichever task logged. No allocation, no
// blocking call, no printf: a bounded copy under the spinlock.
void ringPut(const char *text)
{
    const bool inIsr = xPortInIsrContext();
    if (inIsr)
    {
        portENTER_CRITICAL_ISR(&s_ringMux);
    }
    else
    {
        portENTER_CRITICAL(&s_ringMux);
    }

    // begin() leaves the ring valid, so this only fails if the ring was never
    // armed — in which case the hook is not installed either and we cannot be
    // here. Cheap insurance against writing through a wild s_ring.next.
    if (s_ring.magic == kRingMagic && s_ring.next < RemoteLog::kLastLines)
    {
        char  *slot = s_ring.lines[s_ring.next];
        size_t i    = 0;
        for (; i + 1 < RemoteLog::kLineLen && text[i] != '\0'; i++)
        {
            slot[i] = text[i];
        }
        // Zero the tail as well as terminating: the checksum then covers only
        // bytes this firmware wrote, and no fragment of an older line is left
        // behind the NUL to be published after a truncating write.
        for (size_t j = i; j < RemoteLog::kLineLen; j++)
        {
            slot[j] = '\0';
        }

        s_ring.next = (s_ring.next + 1) % RemoteLog::kLastLines;
        if (s_ring.count < RemoteLog::kLastLines)
        {
            s_ring.count++;
        }
        s_ring.checksum = ringChecksum(s_ring);
    }

    if (inIsr)
    {
        portEXIT_CRITICAL_ISR(&s_ringMux);
    }
    else
    {
        portEXIT_CRITICAL(&s_ringMux);
    }
}

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
// "Treadmill:" is the ESP-IDF tag of every TreadmillHandler line, so the whole
// connect sequence (service found, characteristic, keepalive, subscribed) and
// any kick phase come through, not just the lines that happen to say "connect"
// (spec 4.15).
const char *const kInfoKeywords[] = {
    "connect", "Connect", "kick", "Kick", "Subscribed", "BLE", "Net status", "boot",
    "Treadmill:", "logo", "Heap:",
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
    // Take the pending drop count atomically so two tasks logging at once can
    // never both report (and both clear) the same drops (review 2026-09-06).
    // A drop landing between here and the send is simply reported next time.
    const uint32_t dropped = s_dropped.exchange(0, std::memory_order_relaxed);
    if (dropped != 0)
    {
        snprintf(line.text, sizeof(line.text), "[+%u dropped] %s", (unsigned)dropped, buf);
    }
    else
    {
        snprintf(line.text, sizeof(line.text), "%s", buf);
    }

    // The ring gets the line whether or not the queue does: a full queue is
    // exactly the burst whose tail we most want to see after a crash.
    ringPut(line.text);

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

    if (sent != pdTRUE)
    {
        // The line (and the drops it was about to report) never made it: put
        // the count back plus this one.
        s_dropped.fetch_add(dropped + 1, std::memory_order_relaxed);
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

    // Before anything can log: take the pre-reset tail out of RTC memory and
    // into RAM, then leave the ring exactly as it is (armed, still holding
    // those lines) so a crash during boot still has somewhere to write. Only
    // this copy is published — the ring itself is overwritten by this boot's
    // own lines long before MQTT is up. An invalid ring means power-on garbage
    // or a first boot on this firmware: arm it and report nothing.
    if (ringValid())
    {
        s_savedCount = (uint8_t)s_ring.count;
        for (uint8_t i = 0; i < s_savedCount; i++)
        {
            // Oldest first: in a full ring the oldest line is the one next in
            // line to be overwritten; in a partial ring it is slot 0.
            const uint32_t slot =
                (s_ring.count == kLastLines) ? (s_ring.next + i) % kLastLines : i;
            memcpy(s_saved[i], s_ring.lines[slot], kLineLen);
            s_saved[i][kLineLen - 1] = '\0';
        }
    }
    else
    {
        s_savedCount = 0;
        ringReset();
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

uint8_t lastLines(char (*out)[kLineLen], uint8_t cap)
{
    if (out == nullptr || cap == 0)
    {
        return 0;
    }
    const uint8_t n = (s_savedCount < cap) ? s_savedCount : cap;
    for (uint8_t i = 0; i < n; i++)
    {
        memcpy(out[i], s_saved[i], kLineLen);
        out[i][kLineLen - 1] = '\0';
    }
    return n;
}

void clearLastLines()
{
    s_savedCount = 0;
    portENTER_CRITICAL(&s_ringMux);
    ringReset();
    portEXIT_CRITICAL(&s_ringMux);
}

bool resetWasCrash()
{
    const esp_reset_reason_t reason = esp_reset_reason();
    return reason != ESP_RST_POWERON && reason != ESP_RST_SW;
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
