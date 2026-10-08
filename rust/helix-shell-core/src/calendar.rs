//! The month grid behind the clock. Pure date arithmetic (no time zones: the page says what today is, in the person's own zone), checked for
//! the things a calendar gets wrong: leap years, weeks that start on Sunday or Monday, the days of the neighbouring months that fill the first
//! and last rows, ISO week numbers around New Year, and moving across a year boundary.

use serde::Serialize;

const MIN_YEAR: i32 = 1900;
const MAX_YEAR: i32 = 2200;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Date {
    pub year: i32,
    pub month: u32,
    pub day: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CalendarError {
    BadDate,
    OutOfRange,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Cell {
    pub year: i32,
    pub month: u32,
    pub day: u32,
    pub in_month: bool,
    pub today: bool,
    pub weekend: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Week {
    /// ISO 8601 week number of the week's Thursday (so the rows around New Year count the way the rest of the world does).
    pub iso_week: u32,
    pub days: Vec<Cell>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct MonthView {
    pub year: i32,
    pub month: u32,
    /// 0 = Sunday, 1 = Monday: which weekday the columns start on.
    pub week_start: u32,
    pub weeks: Vec<Week>,
}

pub fn is_leap(year: i32) -> bool {
    (year % 4 == 0 && year % 100 != 0) || year % 400 == 0
}

pub fn days_in_month(year: i32, month: u32) -> u32 {
    match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        _ if is_leap(year) => 29,
        _ => 28,
    }
}

fn valid(date: Date) -> bool {
    (1..=12).contains(&date.month) && date.day >= 1 && date.day <= days_in_month(date.year, date.month)
}

/// Days since 1970-01-01 (proleptic Gregorian).
fn days_from_civil(date: Date) -> i64 {
    let y = i64::from(date.year) - i64::from(date.month <= 2);
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let m = i64::from(date.month);
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + i64::from(date.day) - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

fn civil_from_days(days: i64) -> Date {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    Date { year: (y + i64::from(month <= 2)) as i32, month, day }
}

/// 0 = Sunday ... 6 = Saturday.
pub fn weekday(date: Date) -> u32 {
    (days_from_civil(date) + 4).rem_euclid(7) as u32
}

/// ISO 8601 week number of `date`.
pub fn iso_week(date: Date) -> u32 {
    let days = days_from_civil(date);
    // The week belongs to the year of its Thursday.
    let monday_based = (weekday(date) + 6) % 7;
    let thursday = civil_from_days(days - i64::from(monday_based) + 3);
    let jan1 = days_from_civil(Date { year: thursday.year, month: 1, day: 1 });
    ((days_from_civil(thursday) - jan1) / 7) as u32 + 1
}

/// `(year, month)` moved by `delta` months.
pub fn shift_month(year: i32, month: u32, delta: i32) -> Result<(i32, u32), CalendarError> {
    if !(1..=12).contains(&month) {
        return Err(CalendarError::BadDate);
    }
    let index = i64::from(year) * 12 + i64::from(month) - 1 + i64::from(delta);
    let (y, m) = (index.div_euclid(12), index.rem_euclid(12) as u32 + 1);
    if y < i64::from(MIN_YEAR) || y > i64::from(MAX_YEAR) {
        return Err(CalendarError::OutOfRange);
    }
    Ok((y as i32, m))
}

/// The month `delta` months from `year`/`month`, laid out in whole weeks. `today` marks one cell.
pub fn month_view(year: i32, month: u32, delta: i32, today: Date, week_start: u32) -> Result<MonthView, CalendarError> {
    if !valid(today) || week_start > 1 {
        return Err(CalendarError::BadDate);
    }
    let (year, month) = shift_month(year, month, delta)?;
    let first = Date { year, month, day: 1 };
    let lead = (weekday(first) + 7 - week_start) % 7;
    let first_days = days_from_civil(first);
    let length = days_in_month(year, month);
    let rows = (lead + length).div_ceil(7);
    let today_days = days_from_civil(today);
    let mut weeks = Vec::new();
    for row in 0..rows {
        let mut days = Vec::new();
        for column in 0..7 {
            let offset = i64::from(row * 7 + column) - i64::from(lead);
            let date = civil_from_days(first_days + offset);
            let w = weekday(date);
            days.push(Cell {
                year: date.year,
                month: date.month,
                day: date.day,
                in_month: (0..i64::from(length)).contains(&offset),
                today: first_days + offset == today_days,
                weekend: w == 0 || w == 6,
            });
        }
        // A row is numbered by its Thursday, whichever day the row starts on.
        let thursday = days
            .iter()
            .map(|c| Date { year: c.year, month: c.month, day: c.day })
            .find(|date| weekday(*date) == 4)
            .unwrap_or(first);
        weeks.push(Week { iso_week: iso_week(thursday), days });
    }
    Ok(MonthView { year, month, week_start, weeks })
}

#[cfg(test)]
mod tests {
    use super::*;

    const TODAY: Date = Date { year: 2026, month: 10, day: 8 };

    fn d(year: i32, month: u32, day: u32) -> Date {
        Date { year, month, day }
    }

    #[test]
    fn leap_years_and_month_lengths() {
        assert!(is_leap(2024) && is_leap(2000) && !is_leap(1900) && !is_leap(2026) && !is_leap(2100));
        assert_eq!(days_in_month(2024, 2), 29);
        assert_eq!(days_in_month(2026, 2), 28);
        assert_eq!(days_in_month(2026, 4), 30);
        assert_eq!(days_in_month(2026, 12), 31);
    }

    #[test]
    fn weekdays_are_right_for_known_dates() {
        assert_eq!(weekday(d(1970, 1, 1)), 4, "a Thursday");
        assert_eq!(weekday(d(2026, 10, 8)), 4, "today is a Thursday");
        assert_eq!(weekday(d(2000, 2, 29)), 2, "a Tuesday");
        assert_eq!(weekday(d(2024, 2, 29)), 4);
        assert_eq!(weekday(d(1999, 12, 31)), 5);
        assert_eq!(weekday(d(2021, 1, 3)), 0, "a Sunday");
    }

    #[test]
    fn day_counts_round_trip_across_centuries() {
        for date in [d(1900, 1, 1), d(1969, 12, 31), d(1970, 1, 1), d(2000, 2, 29), d(2026, 10, 8), d(2100, 3, 1), d(2200, 12, 31)] {
            assert_eq!(civil_from_days(days_from_civil(date)), date);
        }
        assert_eq!(days_from_civil(d(1970, 1, 2)) - days_from_civil(d(1970, 1, 1)), 1);
        assert_eq!(days_from_civil(d(2024, 3, 1)) - days_from_civil(d(2024, 2, 28)), 2, "a leap day is counted");
    }

    #[test]
    fn iso_weeks_follow_the_thursday_rule() {
        assert_eq!(iso_week(d(2026, 10, 8)), 41);
        assert_eq!(iso_week(d(2021, 1, 3)), 53, "the last days of 2020 belong to week 53");
        assert_eq!(iso_week(d(2021, 1, 4)), 1);
        assert_eq!(iso_week(d(2024, 12, 30)), 1, "a Monday that belongs to next year's week 1");
        assert_eq!(iso_week(d(2026, 1, 1)), 1);
        assert_eq!(iso_week(d(2020, 12, 31)), 53);
        assert_eq!(iso_week(d(2016, 1, 1)), 53);
    }

    #[test]
    fn moving_between_months_crosses_year_boundaries() {
        assert_eq!(shift_month(2026, 10, 0), Ok((2026, 10)));
        assert_eq!(shift_month(2026, 12, 1), Ok((2027, 1)));
        assert_eq!(shift_month(2026, 1, -1), Ok((2025, 12)));
        assert_eq!(shift_month(2026, 10, 14), Ok((2027, 12)));
        assert_eq!(shift_month(2026, 10, -22), Ok((2024, 12)));
        assert_eq!(shift_month(2026, 10, 120), Ok((2036, 10)));
        assert_eq!(shift_month(1900, 1, -1), Err(CalendarError::OutOfRange));
        assert_eq!(shift_month(2200, 12, 1), Err(CalendarError::OutOfRange));
        assert_eq!(shift_month(2026, 13, 0), Err(CalendarError::BadDate));
        assert_eq!(shift_month(2026, 0, 0), Err(CalendarError::BadDate));
    }

    #[test]
    fn october_2026_starting_on_sunday_and_on_monday() {
        // 1 October 2026 is a Thursday.
        let sunday = month_view(2026, 10, 0, TODAY, 0).unwrap();
        assert_eq!(sunday.weeks.len(), 5);
        let first_row: Vec<(u32, bool)> = sunday.weeks[0].days.iter().map(|c| (c.day, c.in_month)).collect();
        assert_eq!(first_row, [(27, false), (28, false), (29, false), (30, false), (1, true), (2, true), (3, true)]);
        let monday = month_view(2026, 10, 0, TODAY, 1).unwrap();
        let first_row: Vec<(u32, bool)> = monday.weeks[0].days.iter().map(|c| (c.day, c.in_month)).collect();
        assert_eq!(first_row, [(28, false), (29, false), (30, false), (1, true), (2, true), (3, true), (4, true)]);
        let last = sunday.weeks.last().unwrap();
        assert_eq!(last.days.last().map(|c| (c.month, c.day)), Some((10, 31)), "31 October is a Saturday: the grid ends exactly there");
        let november = month_view(2026, 11, 0, TODAY, 0).unwrap();
        let first_row: Vec<(u32, bool)> = november.weeks[0].days.iter().map(|c| (c.day, c.in_month)).collect();
        assert_eq!(first_row, [(1, true), (2, true), (3, true), (4, true), (5, true), (6, true), (7, true)], "1 November is a Sunday");
        let december = month_view(2026, 12, 0, TODAY, 0).unwrap();
        assert_eq!(december.weeks.last().unwrap().days.last().map(|c| (c.month, c.day)), Some((1, 2)), "the last row is filled from January");
    }

    #[test]
    fn exactly_one_cell_is_today_and_only_in_the_month_shown() {
        let view = month_view(2026, 10, 0, TODAY, 0).unwrap();
        let todays: Vec<&Cell> = view.weeks.iter().flat_map(|w| &w.days).filter(|c| c.today).collect();
        assert_eq!(todays.len(), 1);
        assert_eq!((todays[0].month, todays[0].day), (10, 8));
        let next = month_view(2026, 10, 1, TODAY, 0).unwrap();
        assert!(next.weeks.iter().flat_map(|w| &w.days).all(|c| !c.today), "not in November's grid");
        let showing_edge = month_view(2026, 9, 0, d(2026, 10, 1), 0).unwrap();
        assert_eq!(showing_edge.weeks.iter().flat_map(|w| &w.days).filter(|c| c.today).count(), 1, "a neighbouring month's cell can be today");
    }

    #[test]
    fn a_grid_is_whole_weeks_and_covers_every_day_once() {
        for (year, month) in [(2026, 2), (2024, 2), (2026, 10), (2021, 8), (2025, 3), (1999, 12)] {
            for start in [0, 1] {
                let view = month_view(year, month, 0, TODAY, start).unwrap();
                assert!((4..=6).contains(&view.weeks.len()), "{year}-{month}");
                assert!(view.weeks.iter().all(|w| w.days.len() == 7));
                let inside: Vec<u32> = view.weeks.iter().flat_map(|w| &w.days).filter(|c| c.in_month).map(|c| c.day).collect();
                assert_eq!(inside, (1..=days_in_month(year, month)).collect::<Vec<_>>());
            }
        }
        assert_eq!(month_view(2026, 2, 0, TODAY, 0).unwrap().weeks.len(), 4, "a 28-day February starting on a Sunday fits four Sunday weeks");
        assert_eq!(month_view(2026, 2, 0, TODAY, 1).unwrap().weeks.len(), 5, "but spills into a fifth Monday week");
    }

    #[test]
    fn weekends_and_week_numbers_are_marked() {
        let view = month_view(2026, 10, 0, TODAY, 0).unwrap();
        let row = &view.weeks[1];
        assert_eq!(row.days.iter().map(|c| c.weekend).collect::<Vec<_>>(), [true, false, false, false, false, false, true]);
        assert_eq!(view.weeks.iter().map(|w| w.iso_week).collect::<Vec<_>>(), [40, 41, 42, 43, 44], "a row is numbered by its Thursday");
        let january = month_view(2021, 1, 0, TODAY, 1).unwrap();
        assert_eq!(january.weeks[0].iso_week, 53, "Monday 28 December 2020 starts week 53");
        assert_eq!(january.weeks[1].iso_week, 1);
    }

    #[test]
    fn nonsense_input_is_refused() {
        assert_eq!(month_view(2026, 10, 0, d(2026, 2, 30), 0), Err(CalendarError::BadDate));
        assert_eq!(month_view(2026, 10, 0, d(2026, 13, 1), 0), Err(CalendarError::BadDate));
        assert_eq!(month_view(2026, 10, 0, d(2026, 0, 1), 0), Err(CalendarError::BadDate));
        assert_eq!(month_view(2026, 10, 0, TODAY, 2), Err(CalendarError::BadDate));
        assert_eq!(month_view(2026, 10, 100_000, TODAY, 0), Err(CalendarError::OutOfRange));
        assert_eq!(month_view(2026, 10, i32::MIN, TODAY, 0), Err(CalendarError::OutOfRange));
        assert_eq!(month_view(2026, 10, i32::MAX, TODAY, 0), Err(CalendarError::OutOfRange));
        assert!(month_view(2026, 2, 0, d(2024, 2, 29), 0).is_ok(), "a leap day is a real date");
        assert!(month_view(2026, 2, 0, d(2026, 2, 29), 0).is_err(), "and not in a common year");
    }
}
