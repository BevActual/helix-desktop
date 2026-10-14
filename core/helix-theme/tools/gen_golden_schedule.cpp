// Records day_night_schedule::evaluate for a matrix of fake local times.
// The translation unit includes the implementation and shims localtime_r.
#include <ctime>
static std::tm g_fake{};
static long g_fakeOffset = 0;
static std::tm* fake_localtime_r(const std::time_t*, std::tm* out) {
  *out = g_fake;
  out->tm_gmtoff = g_fakeOffset;
  return out;
}
#define localtime_r fake_localtime_r
#include "system/day_night_schedule.cpp"
#undef localtime_r

#include <cstdio>

int main() {
  struct Place { const char* name; double lat, lon; long offset; };
  const Place places[] = {{"london", 51.5, -0.12, 0}, {"sydney", -33.9, 151.2, 39600}, {"quito", -0.2, -78.5, -18000},
                          {"tromso", 69.6, 18.9, 3600}, {"mcmurdo", -77.8, 166.7, 43200}};
  const int days[] = {1, 80, 172, 266, 355};
  const int minutes[] = {0, 59, 300, 420, 600, 720, 1000, 1200, 1439};
  for (const auto& p : places)
    for (int d : days)
      for (int m : minutes)
        for (int s : {0, 37}) {
          g_fake = {};
          g_fake.tm_yday = d - 1;
          g_fake.tm_hour = m / 60;
          g_fake.tm_min = m % 60;
          g_fake.tm_sec = s;
          g_fakeOffset = p.offset;
          LocationConfig cfg;
          cfg.latitude = p.lat;
          cfg.longitude = p.lon;
          const auto e = day_night_schedule::evaluate(cfg, std::nullopt, std::nullopt);
          std::printf("coords\t%s\t%d\t%d\t%d\t%d\t%lld\t%lld\n", p.name, d, m, s, e.night ? 1 : 0,
                      static_cast<long long>(e.untilBoundary.count()), static_cast<long long>(e.sinceBoundary.count()));
        }
  // Manual schedules (including wrap-around and invalid clocks) and the no-coordinates case.
  struct Manual { const char* sunset; const char* sunrise; bool custom; };
  const Manual manuals[] = {{"22:00", "06:30", true}, {"05:00", "17:45", true}, {"7:00", "06:00", true},
                            {"25:00", "06:00", true}, {"21:00", "07:00", false}};
  for (const auto& mc : manuals)
    for (int m : {0, 359, 360, 389, 390, 1019, 1020, 1260, 1319, 1320, 1439}) {
      g_fake = {};
      g_fake.tm_hour = m / 60;
      g_fake.tm_min = m % 60;
      g_fake.tm_sec = 11;
      LocationConfig cfg;
      cfg.customSchedule = mc.custom;
      cfg.sunset = mc.sunset;
      cfg.sunrise = mc.sunrise;
      const auto e = day_night_schedule::evaluate(cfg, std::nullopt, std::nullopt);
      std::printf("manual\t%s\t%s\t%d\t%d\t%d\t%lld\t%lld\n", mc.sunset, mc.sunrise, mc.custom ? 1 : 0, m,
                  e.night ? 1 : 0, static_cast<long long>(e.untilBoundary.count()),
                  static_cast<long long>(e.sinceBoundary.count()));
    }
}
