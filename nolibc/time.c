/*
 * time.c (nolibc) - time(), gmtime(), localtime() and strftime().
 *
 * Local time comes from the zoneinfo database like the C library's:
 * $TZ (a zone name such as "America/Chicago", or "UTC") or else
 * /etc/localtime, in TZif format.  Without one, local time is UTC.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "linux_sys.h"

time_t time(time_t *t) {
    struct k_timespec ts;
    time_t now;
    sys2(SYS_clock_gettime, K_CLOCK_REALTIME, (long)&ts);
    now = ts.tv_sec;
    if (t) *t = now;
    return now;
}

/* days since 1970-01-01 -> civil date (Howard Hinnant's algorithm) */
static void civil(long days, int *y, int *m, int *d) {
    long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static struct tm tmbuf;

static struct tm *breakdown(long t, int isdst) {
    long days = t / 86400, rem = t % 86400;
    int y, m, d;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (rem < 0) { rem += 86400; days--; }
    civil(days, &y, &m, &d);
    tmbuf.tm_year = y - 1900;
    tmbuf.tm_mon = m - 1;
    tmbuf.tm_mday = d;
    tmbuf.tm_hour = (int)(rem / 3600);
    tmbuf.tm_min = (int)(rem / 60 % 60);
    tmbuf.tm_sec = (int)(rem % 60);
    tmbuf.tm_wday = (int)(((days % 7) + 11) % 7); /* 1970-01-01 was a Thursday */
    tmbuf.tm_yday = cum[m - 1] + d - 1 + (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    tmbuf.tm_isdst = isdst;
    return &tmbuf;
}

struct tm *gmtime(const time_t *t) { return breakdown(*t, 0); }

static long be32(const unsigned char *p) {
    return (long)(int)((unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]);
}

/* ---- POSIX TZ strings: "std offset [dst [offset] [,start[/time],end[/time]]]" ---- */

typedef struct {
    char kind; /* 'J' (1..365, no Feb 29), 'D' (0..365), 'M' (month.week.day) */
    int a, b, c;
    long time; /* seconds after local midnight */
} TzDate;

typedef struct {
    long std_off, dst_off; /* seconds east of UTC */
    int has_dst;
    TzDate start, end;
} TzRule;

static long days_from_civil(long y, int m, int d) {
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static const char *tz_name(const char *s) {
    const char *b = s;
    if (*s == '<') {
        while (*s && *s != '>') s++;
        return *s == '>' && s - b >= 4 ? s + 1 : NULL;
    }
    while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) s++;
    return s - b >= 3 ? s : NULL;
}

/* [+-]hh[:mm[:ss]] -> seconds; NULL on error */
static const char *tz_hms(const char *s, long *out) {
    int neg = 0, part = 0;
    long v[3] = {0, 0, 0};
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    if (*s < '0' || *s > '9') return NULL;
    for (;;) {
        while (*s >= '0' && *s <= '9') v[part] = v[part] * 10 + (*s++ - '0');
        if (*s != ':' || part == 2 || s[1] < '0' || s[1] > '9') break;
        s++;
        part++;
    }
    *out = (v[0] * 3600 + v[1] * 60 + v[2]) * (neg ? -1 : 1);
    return s;
}

static const char *tz_date(const char *s, TzDate *d) {
    long n;
    d->time = 7200;
    if (*s == 'M') {
        int i, f[3] = {0, 0, 0};
        s++;
        for (i = 0; i < 3; i++) {
            if (*s < '0' || *s > '9') return NULL;
            while (*s >= '0' && *s <= '9') f[i] = f[i] * 10 + (*s++ - '0');
            if (i < 2 && *s++ != '.') return NULL;
        }
        if (f[0] < 1 || f[0] > 12 || f[1] < 1 || f[1] > 5 || f[2] > 6) return NULL;
        d->kind = 'M';
        d->a = f[0];
        d->b = f[1];
        d->c = f[2];
    } else {
        d->kind = 'D';
        if (*s == 'J') { d->kind = 'J'; s++; }
        if (*s < '0' || *s > '9') return NULL;
        for (n = 0; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
        if (n > 365 || (d->kind == 'J' && n < 1)) return NULL;
        d->a = (int)n;
    }
    if (*s == '/') {
        s = tz_hms(s + 1, &d->time);
        if (!s) return NULL;
    }
    return s;
}

static int tz_parse(const char *s, TzRule *r) {
    long off;
    s = tz_name(s);
    if (!s || !(s = tz_hms(s, &off))) return 0;
    r->std_off = -off;
    r->has_dst = 0;
    if (!*s) return 1;
    if (!(s = tz_name(s))) return 0;
    r->has_dst = 1;
    r->dst_off = r->std_off + 3600;
    if (*s && *s != ',') {
        if (!(s = tz_hms(s, &off))) return 0;
        r->dst_off = -off;
    }
    if (!*s) { /* no rules given: the traditional US ones */
        s = ",M3.2.0,M11.1.0";
    }
    if (*s != ',' || !(s = tz_date(s + 1, &r->start))) return 0;
    if (*s != ',' || !(s = tz_date(s + 1, &r->end))) return 0;
    return *s == 0;
}

static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* local seconds since the epoch at which date rule d fires in year y */
static long tz_when(const TzDate *d, long y) {
    long day0 = days_from_civil(y, 1, 1), day;
    if (d->kind == 'J') day = day0 + d->a - 1 + (is_leap(y) && d->a >= 60);
    else if (d->kind == 'D') day = day0 + d->a;
    else {
        static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        long first = days_from_civil(y, d->a, 1);
        int wd = (int)(((first % 7) + 11) % 7), len = mdays[d->a - 1] + (d->a == 2 && is_leap(y));
        int dom = 1 + (d->c - wd + 7) % 7 + (d->b - 1) * 7;
        while (dom > len) dom -= 7;
        day = first + dom - 1;
    }
    return day * 86400 + d->time;
}

static long tz_rule_offset(const TzRule *r, long t, int *isdst) {
    long y, start, end, days = t / 86400;
    int m, d, yy;
    *isdst = 0;
    if (!r->has_dst) return r->std_off;
    if (t % 86400 < 0) days--;
    civil(days, &yy, &m, &d);
    y = yy;
    start = tz_when(&r->start, y) - r->std_off; /* given in standard time */
    end = tz_when(&r->end, y) - r->dst_off;     /* given in daylight time */
    if (start < end) *isdst = t >= start && t < end;
    else *isdst = !(t >= end && t < start); /* southern hemisphere */
    return *isdst ? r->dst_off : r->std_off;
}

/* ---- TZif files ---- */

static long be64(const unsigned char *p) {
    return (long)((unsigned long)(unsigned)be32(p) << 32 | (unsigned)be32(p + 4));
}

/* UTC offset (seconds) of the local zone at time t: TZ names a zone file
   (or a POSIX rule string); without TZ, /etc/localtime. */
static long zone_offset(long t, int *isdst) {
    static unsigned char data[262144];
    char path[512];
    const char *tz = getenv("TZ");
    long fd, n = 0, r, timecnt, typecnt, i, type = 0;
    const unsigned char *trans, *idx, *types;
    TzRule rule;
    *isdst = 0;
    if (tz && (!*tz || !strcmp(tz, "UTC") || !strcmp(tz, "UTC0") || !strcmp(tz, ":UTC"))) return 0;
    if (tz && *tz == ':') tz++;
    if (tz && *tz && strlen(tz) < 400) {
        if (*tz == '/') strcpy(path, tz);
        else {
            strcpy(path, "/usr/share/zoneinfo/");
            strcat(path, tz);
        }
    } else {
        strcpy(path, "/etc/localtime");
    }
    fd = sys3(SYS_open, (long)path, K_O_RDONLY | K_O_CLOEXEC, 0);
    if (fd < 0) return (tz && tz_parse(tz, &rule)) ? tz_rule_offset(&rule, t, isdst) : 0;
    while (n < (long)sizeof data && (r = sys3(SYS_read, fd, (long)(data + n), (long)sizeof data - n)) > 0) n += r;
    sys1(SYS_close, fd);
    if (n < 44 || memcmp(data, "TZif", 4) != 0) return (tz && tz_parse(tz, &rule)) ? tz_rule_offset(&rule, t, isdst) : 0;
    {
        /* header counts: isutcnt isstdcnt leapcnt timecnt typecnt charcnt */
        long isut = be32(data + 20), isstd = be32(data + 24), leap = be32(data + 28);
        long charcnt = be32(data + 40);
        const unsigned char *hdr = data, *footer = NULL;
        int tsize = 4;
        timecnt = be32(data + 32);
        typecnt = be32(data + 36);
        if (data[4] >= '2') {
            /* skip the 32-bit block; use the 64-bit one that follows */
            long skip = 44 + timecnt * 5 + typecnt * 6 + charcnt + leap * 8 + isstd + isut, end;
            if (skip + 44 > n || memcmp(data + skip, "TZif", 4) != 0) return 0;
            hdr = data + skip;
            isut = be32(hdr + 20);
            isstd = be32(hdr + 24);
            leap = be32(hdr + 28);
            timecnt = be32(hdr + 32);
            typecnt = be32(hdr + 36);
            charcnt = be32(hdr + 40);
            tsize = 8;
            end = skip + 44 + timecnt * 9 + typecnt * 6 + charcnt + leap * 12 + isstd + isut;
            if (end < n && data[end] == '\n') footer = data + end + 1;
        }
        trans = hdr + 44;
        idx = trans + timecnt * tsize;
        types = idx + timecnt;
        if (typecnt <= 0 || types + typecnt * 6 > data + n) return 0;
        for (i = 0; i < timecnt; i++) {
            long tt = tsize == 8 ? be64(trans + i * 8) : be32(trans + i * 4);
            if (tt > t) break;
            type = idx[i];
        }
        /* after the last transition the footer rule (if any) applies */
        if (footer && i == timecnt) {
            char rs[128];
            int k = 0;
            while (footer + k < data + n && footer[k] != '\n' && k < (int)sizeof rs - 1) {
                rs[k] = (char)footer[k];
                k++;
            }
            rs[k] = 0;
            if (k > 0 && tz_parse(rs, &rule)) return tz_rule_offset(&rule, t, isdst);
        }
    }
    if (i == 0) {
        /* before the first transition: first standard-time type */
        for (type = 0; type < typecnt && types[type * 6 + 4]; type++)
            ;
        if (type == typecnt) type = 0;
    }
    if (type >= typecnt) return 0;
    *isdst = types[type * 6 + 4];
    return be32(types + type * 6);
}

struct tm *localtime(const time_t *t) {
    int isdst;
    long off = zone_offset(*t, &isdst);
    return breakdown(*t + off, isdst);
}

size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm) {
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December"};
    size_t n = 0;
    char tmp[32];
    for (; *fmt; fmt++) {
        const char *s = tmp;
        tmp[0] = 0;
        if (*fmt != '%') {
            tmp[0] = *fmt;
            tmp[1] = 0;
        } else {
            int v = -1, w = 2;
            char padc = '0';
            switch (*++fmt) {
            case 'a': strncpy(tmp, days[tm->tm_wday % 7], 3); tmp[3] = 0; break;
            case 'A': s = days[tm->tm_wday % 7]; break;
            case 'b': case 'h': strncpy(tmp, months[tm->tm_mon % 12], 3); tmp[3] = 0; break;
            case 'B': s = months[tm->tm_mon % 12]; break;
            case 'd': v = tm->tm_mday; break;
            case 'e': v = tm->tm_mday; padc = ' '; break;
            case 'H': v = tm->tm_hour; break;
            case 'I': v = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12; break;
            case 'M': v = tm->tm_min; break;
            case 'S': v = tm->tm_sec; break;
            case 'm': v = tm->tm_mon + 1; break;
            case 'y': v = tm->tm_year % 100; break;
            case 'Y': v = tm->tm_year + 1900; w = 4; break;
            case 'j': v = tm->tm_yday + 1; w = 3; break;
            case 'p': s = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case '%': s = "%"; break;
            case 0: fmt--; break;
            default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
            }
            if (v >= 0) {
                int i;
                char *p = tmp + 16;
                *p = 0;
                do { *--p = (char)('0' + v % 10); v /= 10; } while (v);
                for (i = (int)(tmp + 16 - p); i < w; i++) *--p = padc;
                s = p;
            }
        }
        while (*s) {
            if (n + 1 >= max) return 0;
            buf[n++] = *s++;
        }
    }
    if (n >= max) return 0;
    buf[n] = 0;
    return n;
}
