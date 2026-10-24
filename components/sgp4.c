/* SGP4 for near-earth orbits (see sgp4.h). The names and the order of the
 * terms follow Vallado's reference code (sgp4unit), so the two can be
 * compared line by line; test/host/test_sgp4.c checks it against the
 * python-sgp4 package, which wraps that code. */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sgp4.h"

#define TWOPI (2.0 * M_PI)
#define DEG   (M_PI / 180.0)

/* WGS-72, as the elements are fitted with. */
#define RE_KM 6378.135
#define MU    398600.8
#define J2    0.001082616
#define J3    -0.00000253881
#define J4    -0.00000165597
#define J3OJ2 (J3 / J2)
#define X2O3  (2.0 / 3.0)

static double xke(void)
{
    return 60.0 / sqrt(RE_KM * RE_KM * RE_KM / MU);
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian). */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Columns `from`..`to` (1-based, as in the TLE format) of `line` as a number. */
static double field(const char *line, int from, int to)
{
    char buf[24];
    const int n = to - from + 1;
    memcpy(buf, line + from - 1, (size_t)n);
    buf[n] = '\0';
    return strtod(buf, NULL);
}

/* A TLE line: at least 69 characters, the given line number, the checksum
 * in column 69 right. */
static bool line_ok(const char *line, char number)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        len--;
    }
    if (len < 69 || line[0] != number || line[1] != ' ' || !isdigit((unsigned char)line[68])) {
        return false;
    }
    int sum = 0;
    for (int i = 0; i < 68; i++) {
        if (isdigit((unsigned char)line[i])) {
            sum += line[i] - '0';
        } else if (line[i] == '-') {
            sum += 1;
        }
    }
    return sum % 10 == line[68] - '0';
}

/* A number in the TLE's packed form: "+12345-3" or " 12345-3" is 0.12345e-3. */
static double packed(const char *line, int from)
{
    const double m = field(line, from + 1, from + 5) * 1e-5;
    const int e = (int)field(line, from + 6, from + 7);
    return (line[from - 1] == '-' ? -m : m) * pow(10.0, e);
}

/* sgp4init and initl, for near-earth orbits. */
static bool init(sgp4_t *s)
{
    const double ke = xke();
    const double ss = 78.0 / RE_KM + 1.0;
    const double qzms2t = pow((120.0 - 78.0) / RE_KM, 4);

    /* initl */
    const double eccsq = s->ecco * s->ecco;
    const double omeosq = 1.0 - eccsq;
    const double rteosq = sqrt(omeosq);
    const double cosio = cos(s->inclo);
    const double cosio2 = cosio * cosio;
    const double ak = pow(ke / s->no_kozai, X2O3);
    const double d1 = 0.75 * J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
    double del = d1 / (ak * ak);
    const double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
    del = d1 / (adel * adel);
    s->no_unkozai = s->no_kozai / (1.0 + del);
    const double ao = pow(ke / s->no_unkozai, X2O3);
    s->ao = ao;
    const double sinio = sin(s->inclo);
    const double po = ao * omeosq;
    s->con42 = 1.0 - 5.0 * cosio2;
    s->con41 = -s->con42 - cosio2 - cosio2;
    const double posq = po * po;
    const double rp = ao * (1.0 - s->ecco);
    if (TWOPI / s->no_unkozai >= 225.0 || omeosq <= 0.0 || s->no_unkozai <= 0.0) {
        return false; /* deep space, or not an orbit */
    }

    /* sgp4init */
    s->isimp = rp < 220.0 / RE_KM + 1.0;
    double sfour = ss, qzms24 = qzms2t;
    const double perige = (rp - 1.0) * RE_KM;
    if (perige < 156.0) {
        sfour = perige - 78.0;
        if (perige < 98.0) {
            sfour = 20.0;
        }
        qzms24 = pow((120.0 - sfour) / RE_KM, 4);
        sfour = sfour / RE_KM + 1.0;
    }
    const double pinvsq = 1.0 / posq;
    const double tsi = 1.0 / (ao - sfour);
    s->eta = ao * s->ecco * tsi;
    const double etasq = s->eta * s->eta;
    const double eeta = s->ecco * s->eta;
    const double psisq = fabs(1.0 - etasq);
    const double coef = qzms24 * pow(tsi, 4);
    const double coef1 = coef / pow(psisq, 3.5);
    const double cc2 = coef1 * s->no_unkozai *
                       (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
                        0.375 * J2 * tsi / psisq * s->con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
    s->cc1 = s->bstar * cc2;
    double cc3 = 0.0;
    if (s->ecco > 1.0e-4) {
        cc3 = -2.0 * coef * tsi * J3OJ2 * s->no_unkozai * sinio / s->ecco;
    }
    s->x1mth2 = 1.0 - cosio2;
    s->cc4 = 2.0 * s->no_unkozai * coef1 * ao * omeosq *
             (s->eta * (2.0 + 0.5 * etasq) + s->ecco * (0.5 + 2.0 * etasq) -
              J2 * tsi / (ao * psisq) *
                  (-3.0 * s->con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
                   0.75 * s->x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * cos(2.0 * s->argpo)));
    s->cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
    const double cosio4 = cosio2 * cosio2;
    const double temp1 = 1.5 * J2 * pinvsq * s->no_unkozai;
    const double temp2 = 0.5 * temp1 * J2 * pinvsq;
    const double temp3 = -0.46875 * J4 * pinvsq * pinvsq * s->no_unkozai;
    s->mdot = s->no_unkozai + 0.5 * temp1 * rteosq * s->con41 +
              0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
    s->argpdot = -0.5 * temp1 * s->con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
                 temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
    const double xhdot1 = -temp1 * cosio;
    s->nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
    s->omgcof = s->bstar * cc3 * cos(s->argpo);
    s->xmcof = 0.0;
    if (s->ecco > 1.0e-4) {
        s->xmcof = -X2O3 * coef * s->bstar / eeta;
    }
    s->nodecf = 3.5 * omeosq * xhdot1 * s->cc1;
    s->t2cof = 1.5 * s->cc1;
    const double cosp1 = fabs(cosio + 1.0) > 1.5e-12 ? 1.0 + cosio : 1.5e-12;
    s->xlcof = -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / cosp1;
    s->aycof = -0.5 * J3OJ2 * sinio;
    s->delmo = pow(1.0 + s->eta * cos(s->mo), 3);
    s->sinmao = sin(s->mo);
    s->x7thm1 = 7.0 * cosio2 - 1.0;
    if (!s->isimp) {
        const double cc1sq = s->cc1 * s->cc1;
        s->d2 = 4.0 * ao * tsi * cc1sq;
        const double temp = s->d2 * tsi * s->cc1 / 3.0;
        s->d3 = (17.0 * ao + sfour) * temp;
        s->d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s->cc1;
        s->t3cof = s->d2 + 2.0 * cc1sq;
        s->t4cof = 0.25 * (3.0 * s->d3 + s->cc1 * (12.0 * s->d2 + 10.0 * cc1sq));
        s->t5cof = 0.2 * (3.0 * s->d4 + 12.0 * s->cc1 * s->d3 + 6.0 * s->d2 * s->d2 +
                          15.0 * cc1sq * (2.0 * s->d2 + cc1sq));
    }
    return true;
}

bool sgp4_parse(const char *line1, const char *line2, sgp4_t *s)
{
    if (line1 == NULL || line2 == NULL || !line_ok(line1, '1') || !line_ok(line2, '2')) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->catnr = (int)field(line1, 3, 7);
    if ((int)field(line2, 3, 7) != s->catnr) {
        return false;
    }
    const int yy = (int)field(line1, 19, 20);
    const double doy = field(line1, 21, 32);
    const int year = yy < 57 ? 2000 + yy : 1900 + yy;
    s->epoch = (double)days_from_civil(year, 1, 1) * 86400.0 + (doy - 1.0) * 86400.0;
    s->bstar = packed(line1, 54);
    s->inclo = field(line2, 9, 16) * DEG;
    s->nodeo = field(line2, 18, 25) * DEG;
    s->ecco = field(line2, 27, 33) * 1e-7;
    s->argpo = field(line2, 35, 42) * DEG;
    s->mo = field(line2, 44, 51) * DEG;
    s->no_kozai = field(line2, 53, 63) * TWOPI / 1440.0;
    if (s->no_kozai <= 0.0 || s->ecco >= 1.0) {
        return false;
    }
    return init(s);
}

bool sgp4_parse_text(const char *text, sgp4_t *s)
{
    char lines[2][72];
    int have = 0; /* the line wanted next: 0 for line 1, 1 for line 2 */
    for (const char *p = text; p != NULL && *p != '\0';) {
        const char *eol = strchr(p, '\n');
        size_t n = eol != NULL ? (size_t)(eol - p) : strlen(p);
        if (n > 0 && p[n - 1] == '\r') {
            n--;
        }
        if (n >= 69 && n < sizeof(lines[0]) && p[0] == (have == 0 ? '1' : '2') && p[1] == ' ') {
            memcpy(lines[have], p, n);
            lines[have][n] = '\0';
            if (++have == 2) {
                return sgp4_parse(lines[0], lines[1], s);
            }
        } else if (have == 1) {
            have = 0; /* line 2 must follow line 1 */
            continue; /* this one may be a line 1 */
        }
        p = eol != NULL ? eol + 1 : NULL;
    }
    return false;
}

bool sgp4_pos(const sgp4_t *s, double t_epoch, double r[3], double v[3])
{
    const double ke = xke();
    const double vkmpersec = RE_KM * ke / 60.0;
    const double t = (t_epoch - s->epoch) / 60.0; /* minutes from the elements' epoch */

    const double xmdf = s->mo + s->mdot * t;
    const double argpdf = s->argpo + s->argpdot * t;
    const double nodedf = s->nodeo + s->nodedot * t;
    double argpm = argpdf, mm = xmdf;
    const double t2 = t * t;
    double nodem = nodedf + s->nodecf * t2;
    double tempa = 1.0 - s->cc1 * t;
    double tempe = s->bstar * s->cc4 * t;
    double templ = s->t2cof * t2;
    if (!s->isimp) {
        const double delomg = s->omgcof * t;
        const double delm = s->xmcof * (pow(1.0 + s->eta * cos(xmdf), 3) - s->delmo);
        const double temp = delomg + delm;
        mm = xmdf + temp;
        argpm = argpdf - temp;
        const double t3 = t2 * t, t4 = t3 * t;
        tempa = tempa - s->d2 * t2 - s->d3 * t3 - s->d4 * t4;
        tempe = tempe + s->bstar * s->cc5 * (sin(mm) - s->sinmao);
        templ = templ + s->t3cof * t3 + t4 * (s->t4cof + t * s->t5cof);
    }
    double nm = s->no_unkozai;
    double em = s->ecco;
    const double inclm = s->inclo;
    const double am = pow(ke / nm, X2O3) * tempa * tempa;
    nm = ke / pow(am, 1.5);
    em = em - tempe;
    if (em >= 1.0 || em < -0.001 || am < 0.95) {
        return false;
    }
    if (em < 1.0e-6) {
        em = 1.0e-6;
    }
    mm = mm + s->no_unkozai * templ;
    double xlm = mm + argpm + nodem;
    nodem = fmod(nodem, TWOPI);
    argpm = fmod(argpm, TWOPI);
    xlm = fmod(xlm, TWOPI);
    mm = fmod(xlm - argpm - nodem, TWOPI);
    const double sinip = sin(inclm), cosip = cos(inclm);

    /* Long-period periodics. */
    const double axnl = em * cos(argpm);
    double temp = 1.0 / (am * (1.0 - em * em));
    const double aynl = em * sin(argpm) + temp * s->aycof;
    const double xl = mm + argpm + nodem + temp * s->xlcof * axnl;

    /* Kepler's equation. */
    const double u = fmod(xl - nodem, TWOPI);
    double eo1 = u, tem5 = 9999.9, sineo1 = 0.0, coseo1 = 0.0;
    for (int ktr = 1; fabs(tem5) >= 1.0e-12 && ktr <= 10; ktr++) {
        sineo1 = sin(eo1);
        coseo1 = cos(eo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (fabs(tem5) >= 0.95) {
            tem5 = tem5 > 0.0 ? 0.95 : -0.95;
        }
        eo1 = eo1 + tem5;
    }

    /* Short-period periodics. */
    const double ecose = axnl * coseo1 + aynl * sineo1;
    const double esine = axnl * sineo1 - aynl * coseo1;
    const double el2 = axnl * axnl + aynl * aynl;
    const double pl = am * (1.0 - el2);
    if (pl < 0.0) {
        return false;
    }
    const double rl = am * (1.0 - ecose);
    const double rdotl = sqrt(am) * esine / rl;
    const double rvdotl = sqrt(pl) / rl;
    const double betal = sqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    const double sinu = am / rl * (sineo1 - aynl - axnl * temp);
    const double cosu = am / rl * (coseo1 - axnl + aynl * temp);
    double su = atan2(sinu, cosu);
    const double sin2u = (cosu + cosu) * sinu;
    const double cos2u = 1.0 - 2.0 * sinu * sinu;
    temp = 1.0 / pl;
    const double temp1 = 0.5 * J2 * temp;
    const double temp2 = temp1 * temp;
    const double mrt = rl * (1.0 - 1.5 * temp2 * betal * s->con41) + 0.5 * temp1 * s->x1mth2 * cos2u;
    su = su - 0.25 * temp2 * s->x7thm1 * sin2u;
    const double xnode = nodem + 1.5 * temp2 * cosip * sin2u;
    const double xinc = inclm + 1.5 * temp2 * cosip * sinip * cos2u;
    const double mvt = rdotl - nm * temp1 * s->x1mth2 * sin2u / ke;
    const double rvdot = rvdotl + nm * temp1 * (s->x1mth2 * cos2u + 1.5 * s->con41) / ke;

    const double sinsu = sin(su), cossu = cos(su);
    const double snod = sin(xnode), cnod = cos(xnode);
    const double sini = sin(xinc), cosi = cos(xinc);
    const double xmx = -snod * cosi, xmy = cnod * cosi;
    const double ux = xmx * sinsu + cnod * cossu;
    const double uy = xmy * sinsu + snod * cossu;
    const double uz = sini * sinsu;
    const double vx = xmx * cossu - cnod * sinsu;
    const double vy = xmy * cossu - snod * sinsu;
    const double vz = sini * cossu;
    r[0] = mrt * ux * RE_KM;
    r[1] = mrt * uy * RE_KM;
    r[2] = mrt * uz * RE_KM;
    v[0] = (mvt * ux + rvdot * vx) * vkmpersec;
    v[1] = (mvt * uy + rvdot * vy) * vkmpersec;
    v[2] = (mvt * uz + rvdot * vz) * vkmpersec;
    return mrt >= 1.0;
}

double sgp4_gmst(double t)
{
    /* Vallado's gstime, from the Julian date (UT1 taken as UTC). */
    const double jd = t / 86400.0 + 2440587.5;
    const double tut1 = (jd - 2451545.0) / 36525.0;
    double g = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 +
               (876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841; /* seconds */
    g = fmod(g * DEG / 240.0, TWOPI);
    return g < 0.0 ? g + TWOPI : g;
}

bool sgp4_ground(const sgp4_t *s, double t, sgp4_ground_t *g)
{
    double r[3], v[3];
    if (!sgp4_pos(s, t, r, v)) {
        return false;
    }
    /* Into the earth's frame, then geodetic latitude on WGS-84 by a few
     * rounds of the usual iteration. */
    const double gmst = sgp4_gmst(t);
    double lon = atan2(r[1], r[0]) - gmst;
    lon = fmod(lon + M_PI, TWOPI);
    if (lon < 0.0) {
        lon += TWOPI;
    }
    lon -= M_PI;
    const double a = 6378.137, e2 = 0.00669437999014;
    const double p = sqrt(r[0] * r[0] + r[1] * r[1]);
    double lat = atan2(r[2], p), n = a;
    for (int i = 0; i < 5; i++) {
        const double sl = sin(lat);
        n = a / sqrt(1.0 - e2 * sl * sl);
        lat = atan2(r[2] + n * e2 * sl, p);
    }
    const double cl = cos(lat);
    g->alt_km = fabs(cl) > 1e-6 ? p / cl - n : fabs(r[2]) - a * sqrt(1.0 - e2);
    g->lat = lat / DEG;
    g->lon = lon / DEG;
    g->speed_kmh = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * 3600.0;
    return true;
}
