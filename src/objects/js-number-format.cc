// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTL_SUPPORT
#error Internationalization is expected to be enabled.
#endif  // V8_INTL_SUPPORT

#include "src/objects/js-number-format.h"

#include <set>
#include <string>

#include "src/execution/isolate.h"
#include "src/numbers/conversions.h"
#include "src/objects/intl-objects.h"
#include "src/objects/js-number-format-inl.h"
#include "src/objects/managed-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/option-utils.h"
#include "src/strings/char-predicates-inl.h"
#include "unicode/currunit.h"
#include "unicode/locid.h"
#include "unicode/numberformatter.h"
#include "unicode/numberrangeformatter.h"
#include "unicode/numsys.h"
#include "unicode/ucurr.h"
#include "unicode/uloc.h"
#include "unicode/unumberformatter.h"
#include "unicode/uvernum.h"  // for U_ICU_VERSION_MAJOR_NUM

namespace v8 {
namespace internal {

namespace {

// This is to work around ICU's comparison operators not being compliant with
// clang's -Wambiguous-reversed-operator in >=C++20.
#define AVOID_AMBIGUOUS_OP_WARNING(x) *static_cast<icu::UObject*>(&x)

// [[Style]] is one of the values "decimal", "percent", "currency",
// or "unit" identifying the style of the number format.
enum class Style { DECIMAL, PERCENT, CURRENCY, UNIT };

// [[CurrencyDisplay]] is one of the values "code", "symbol", "name",
// or "narrowSymbol" identifying the display of the currency number format.
enum class CurrencyDisplay {
  CODE,
  SYMBOL,
  NAME,
  NARROW_SYMBOL,
};

// [[CurrencySign]] is one of the String values "standard" or "accounting",
// specifying whether to render negative numbers in accounting format, often
// signified by parenthesis. It is only used when [[Style]] has the value
// "currency" and when [[SignDisplay]] is not "never".
enum class CurrencySign {
  STANDARD,
  ACCOUNTING,
};

// [[UnitDisplay]] is one of the String values "short", "narrow", or "long",
// specifying whether to display the unit as a symbol, narrow symbol, or
// localized long name if formatting with the "unit" style. It is
// only used when [[Style]] has the value "unit".
enum class UnitDisplay {
  SHORT,
  NARROW,
  LONG,
};

// [[Notation]] is one of the String values "standard", "scientific",
// "engineering", or "compact", specifying whether the number should be
// displayed without scaling, scaled to the units place with the power of ten
// in scientific notation, scaled to the nearest thousand with the power of
// ten in scientific notation, or scaled to the nearest locale-dependent
// compact decimal notation power of ten with the corresponding compact
// decimal notation affix.

enum class Notation {
  STANDARD,
  SCIENTIFIC,
  ENGINEERING,
  COMPACT,
};

// [[CompactDisplay]] is one of the String values "short" or "long",
// specifying whether to display compact notation affixes in short form ("5K")
// or long form ("5 thousand") if formatting with the "compact" notation. It
// is only used when [[Notation]] has the value "compact".
enum class CompactDisplay {
  SHORT,
  LONG,
};

// [[SignDisplay]] is one of the String values "auto", "always", "never", or
// "exceptZero", specifying whether to show the sign on negative numbers
// only, positive and negative numbers including zero, neither positive nor
// negative numbers, or positive and negative numbers but not zero.
enum class SignDisplay {
  AUTO,
  ALWAYS,
  NEVER,
  EXCEPT_ZERO,
  NEGATIVE,
};

// [[RoundingMode]] is one of the String values "ceil", "floor", "expand",
// "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", or "halfEven",
// specifying the rounding strategy for the number.
// Note: To avoid name conflict with RoundingMode defined in other places,
// prefix with Intl as IntlRoundingMode
enum class IntlRoundingMode {
  CEIL,
  FLOOR,
  EXPAND,
  TRUNC,
  HALF_CEIL,
  HALF_FLOOR,
  HALF_EXPAND,
  HALF_TRUNC,
  HALF_EVEN,
};

// [[TrailingZeroDisplay]] is one of the String values "auto" or
// "stripIfInteger", specifying the strategy for displaying trailing zeros on
// whole number.
enum class TrailingZeroDisplay {
  AUTO,
  STRIP_IF_INTEGER,
};

// [[UseGrouping]] is ....
enum class UseGrouping {
  OFF,
  MIN2,
  AUTO,
  ALWAYS,
};

UNumberUnitWidth ToUNumberUnitWidth(CurrencyDisplay currency_display) {
  switch (currency_display) {
    case CurrencyDisplay::SYMBOL:
      return UNumberUnitWidth::UNUM_UNIT_WIDTH_SHORT;
    case CurrencyDisplay::CODE:
      return UNumberUnitWidth::UNUM_UNIT_WIDTH_ISO_CODE;
    case CurrencyDisplay::NAME:
      return UNumberUnitWidth::UNUM_UNIT_WIDTH_FULL_NAME;
    case CurrencyDisplay::NARROW_SYMBOL:
      return UNumberUnitWidth::UNUM_UNIT_WIDTH_NARROW;
  }
}

UNumberUnitWidth ToUNumberUnitWidth(UnitDisplay unit_display) {
  switch (unit_display) {
    case UnitDisplay::SHORT:
      return UNumberUnitWidth::UNUM_UNIT_WIDTH_SHORT;
    case UnitDisplay::LONG:
      return UNumberUnitWidth::UNUM_UNIT_WIDTH_FULL_NAME;
    case UnitDisplay::NARROW:
      return UNumberUnitWidth::UNUM_UNIT_WIDTH_NARROW;
  }
}

UNumberSignDisplay ToUNumberSignDisplay(SignDisplay sign_display,
                                        CurrencySign currency_sign) {
  switch (sign_display) {
    case SignDisplay::AUTO:
      if (currency_sign == CurrencySign::ACCOUNTING) {
        return UNumberSignDisplay::UNUM_SIGN_ACCOUNTING;
      }
      DCHECK(currency_sign == CurrencySign::STANDARD);
      return UNumberSignDisplay::UNUM_SIGN_AUTO;
    case SignDisplay::NEVER:
      return UNumberSignDisplay::UNUM_SIGN_NEVER;
    case SignDisplay::ALWAYS:
      if (currency_sign == CurrencySign::ACCOUNTING) {
        return UNumberSignDisplay::UNUM_SIGN_ACCOUNTING_ALWAYS;
      }
      DCHECK(currency_sign == CurrencySign::STANDARD);
      return UNumberSignDisplay::UNUM_SIGN_ALWAYS;
    case SignDisplay::EXCEPT_ZERO:
      if (currency_sign == CurrencySign::ACCOUNTING) {
        return UNumberSignDisplay::UNUM_SIGN_ACCOUNTING_EXCEPT_ZERO;
      }
      DCHECK(currency_sign == CurrencySign::STANDARD);
      return UNumberSignDisplay::UNUM_SIGN_EXCEPT_ZERO;
    case SignDisplay::NEGATIVE:
      if (currency_sign == CurrencySign::ACCOUNTING) {
        return UNumberSignDisplay::UNUM_SIGN_ACCOUNTING_NEGATIVE;
      }
      DCHECK(currency_sign == CurrencySign::STANDARD);
      return UNumberSignDisplay::UNUM_SIGN_NEGATIVE;
  }
}

icu::number::Notation ToICUNotation(Notation notation,
                                    CompactDisplay compact_display) {
  switch (notation) {
    case Notation::STANDARD:
      return icu::number::Notation::simple();
    case Notation::SCIENTIFIC:
      return icu::number::Notation::scientific();
    case Notation::ENGINEERING:
      return icu::number::Notation::engineering();
    // 29. If notation is "compact", then
    case Notation::COMPACT:
      // 29. a. Set numberFormat.[[CompactDisplay]] to compactDisplay.
      if (compact_display == CompactDisplay::SHORT) {
        return icu::number::Notation::compactShort();
      }
      DCHECK(compact_display == CompactDisplay::LONG);
      return icu::number::Notation::compactLong();
  }
}

UNumberFormatRoundingMode ToUNumberFormatRoundingMode(
    IntlRoundingMode rounding_mode) {
  switch (rounding_mode) {
    case IntlRoundingMode::CEIL:
      return UNumberFormatRoundingMode::UNUM_ROUND_CEILING;
    case IntlRoundingMode::FLOOR:
      return UNumberFormatRoundingMode::UNUM_ROUND_FLOOR;
    case IntlRoundingMode::EXPAND:
      return UNumberFormatRoundingMode::UNUM_ROUND_UP;
    case IntlRoundingMode::TRUNC:
      return UNumberFormatRoundingMode::UNUM_ROUND_DOWN;
    case IntlRoundingMode::HALF_CEIL:
      return UNumberFormatRoundingMode::UNUM_ROUND_HALF_CEILING;
    case IntlRoundingMode::HALF_FLOOR:
      return UNumberFormatRoundingMode::UNUM_ROUND_HALF_FLOOR;
    case IntlRoundingMode::HALF_EXPAND:
      return UNumberFormatRoundingMode::UNUM_ROUND_HALFUP;
    case IntlRoundingMode::HALF_TRUNC:
      return UNumberFormatRoundingMode::UNUM_ROUND_HALFDOWN;
    case IntlRoundingMode::HALF_EVEN:
      return UNumberFormatRoundingMode::UNUM_ROUND_HALFEVEN;
  }
}

UNumberGroupingStrategy ToUNumberGroupingStrategy(UseGrouping use_grouping) {
  switch (use_grouping) {
    case UseGrouping::OFF:
      return UNumberGroupingStrategy::UNUM_GROUPING_OFF;
    case UseGrouping::MIN2:
      return UNumberGroupingStrategy::UNUM_GROUPING_MIN2;
    case UseGrouping::AUTO:
      return UNumberGroupingStrategy::UNUM_GROUPING_AUTO;
    case UseGrouping::ALWAYS:
      return UNumberGroupingStrategy::UNUM_GROUPING_ON_ALIGNED;
  }
}

std::map<const std::string, icu::MeasureUnit> CreateUnitMap() {
  UErrorCode status = U_ZERO_ERROR;
  int32_t total = icu::MeasureUnit::getAvailable(nullptr, 0, status);
  DCHECK(U_FAILURE(status));
  status = U_ZERO_ERROR;
  std::vector<icu::MeasureUnit> units(total);
  total = icu::MeasureUnit::getAvailable(units.data(), total, status);
  DCHECK(U_SUCCESS(status));
  std::map<const std::string, icu::MeasureUnit> map;
  std::set<std::string> sanctioned(Intl::SanctionedSimpleUnits());
  for (auto it = units.begin(); it != units.end(); ++it) {
    // Need to skip none/percent
    if (sanctioned.count(it->getSubtype()) > 0 &&
        strcmp("none", it->getType()) != 0) {
      map[it->getSubtype()] = *it;
    }
  }
  return map;
}

class UnitFactory {
 public:
  UnitFactory() : map_(CreateUnitMap()) {}
  virtual ~UnitFactory() = default;

  // ecma402 #sec-issanctionedsimpleunitidentifier
  icu::MeasureUnit create(const std::string& unitIdentifier) {
    // 1. If unitIdentifier is in the following list, return true.
    auto found = map_.find(unitIdentifier);
    if (found != map_.end()) {
      return found->second;
    }
    // 2. Return false.
    return icu::MeasureUnit();
  }

 private:
  std::map<const std::string, icu::MeasureUnit> map_;
};

// ecma402 #sec-issanctionedsimpleunitidentifier
icu::MeasureUnit IsSanctionedUnitIdentifier(const std::string& unit) {
  static base::LazyInstance<UnitFactory>::type factory =
      LAZY_INSTANCE_INITIALIZER;
  return factory.Pointer()->create(unit);
}

// ecma402 #sec-iswellformedunitidentifier
Maybe<std::pair<icu::MeasureUnit, icu::MeasureUnit>> IsWellFormedUnitIdentifier(
    Isolate* isolate, const std::string& unit) {
  icu::MeasureUnit result = IsSanctionedUnitIdentifier(unit);
  icu::MeasureUnit none = icu::MeasureUnit();
  // 1. If the result of IsSanctionedUnitIdentifier(unitIdentifier) is true,
  // then
  if (result != AVOID_AMBIGUOUS_OP_WARNING(none)) {
    // a. Return true.
    std::pair<icu::MeasureUnit, icu::MeasureUnit> pair(result, none);
    return Just(pair);
  }
  // 2. If the substring "-per-" does not occur exactly once in unitIdentifier,
  // then
  size_t first_per = unit.find("-per-");
  if (first_per == std::string::npos ||
      unit.find("-per-", first_per + 5) != std::string::npos) {
    // a. Return false.
    return Nothing<std::pair<icu::MeasureUnit, icu::MeasureUnit>>();
  }
  // 3. Let numerator be the substring of unitIdentifier from the beginning to
  // just before "-per-".
  std::string numerator = unit.substr(0, first_per);

  // 4. If the result of IsSanctionedUnitIdentifier(numerator) is false, then
  result = IsSanctionedUnitIdentifier(numerator);
  if (result == AVOID_AMBIGUOUS_OP_WARNING(none)) {
    // a. Return false.
    return Nothing<std::pair<icu::MeasureUnit, icu::MeasureUnit>>();
  }
  // 5. Let denominator be the substring of unitIdentifier from just after
  // "-per-" to the end.
  std::string denominator = unit.substr(first_per + 5);

  // 6. If the result of IsSanctionedUnitIdentifier(denominator) is false, then
  icu::MeasureUnit den_result = IsSanctionedUnitIdentifier(denominator);
  if (den_result == AVOID_AMBIGUOUS_OP_WARNING(none)) {
    // a. Return false.
    return Nothing<std::pair<icu::MeasureUnit, icu::MeasureUnit>>();
  }
  // 7. Return true.
  std::pair<icu::MeasureUnit, icu::MeasureUnit> pair(result, den_result);
  return Just(pair);
}

// ecma-402/#sec-currencydigits
// The currency is expected to an all upper case string value.
int CurrencyDigits(const icu::UnicodeString& currency) {
  UErrorCode status = U_ZERO_ERROR;
  uint32_t fraction_digits = ucurr_getDefaultFractionDigits(
      reinterpret_cast<const UChar*>(currency.getBuffer()), &status);
  // For missing currency codes, default to the most common, 2
  return U_SUCCESS(status) ? fraction_digits : 2;
}

bool IsAToZ(char ch) {
  return base::IsInRange(AsciiAlphaToLower(ch), 'a', 'z');
}

// ecma402/#sec-iswellformedcurrencycode
bool IsWellFormedCurrencyCode(const std::string& currency) {
  // Verifies that the input is a well-formed ISO 4217 currency code.
  // ecma402/#sec-currency-codes
  // 2. If the number of elements in normalized is not 3, return false.
  if (currency.length() != 3) return false;
  // 1. Let normalized be the result of mapping currency to upper case as
  //   described in 6.1.
  //
  // 3. If normalized contains any character that is not in
  // the range "A" to "Z" (U+0041 to U+005A), return false.
  //
  // 4. Return true.
  // Don't uppercase to test. It could convert invalid code into a valid one.
  // For example \u00DFP (Eszett+P) becomes SSP.
  return (IsAToZ(currency[0]) && IsAToZ(currency[1]) && IsAToZ(currency[2]));
}

// Return the style as a String.
Handle<String> StyleAsString(Isolate* isolate, Style style) {
  switch (style) {
    case Style::PERCENT:
      return ReadOnlyRoots(isolate).percent_string_handle();
    case Style::CURRENCY:
      return ReadOnlyRoots(isolate).currency_string_handle();
    case Style::UNIT:
      return ReadOnlyRoots(isolate).unit_string_handle();
    case Style::DECIMAL:
      return ReadOnlyRoots(isolate).decimal_string_handle();
  }
  UNREACHABLE();
}

// Parse the 'currencyDisplay' from the skeleton.
Handle<String> CurrencyDisplayString(Isolate* isolate,
                                     const icu::UnicodeString& skeleton) {
  // Ex: skeleton as
  // "currency/TWD .00 rounding-mode-half-up unit-width-iso-code"
  if (skeleton.indexOf("unit-width-iso-code") >= 0) {
    return ReadOnlyRoots(isolate).code_string_handle();
  }
  // Ex: skeleton as
  // "currency/TWD .00 rounding-mode-half-up unit-width-full-name;"
  if (skeleton.indexOf("unit-width-full-name") >= 0) {
    return ReadOnlyRoots(isolate).name_string_handle();
  }
  // Ex: skeleton as
  // "currency/TWD .00 rounding-mode-half-up unit-width-narrow;
  if (skeleton.indexOf("unit-width-narrow") >= 0) {
    return ReadOnlyRoots(isolate).narrowSymbol_string_handle();
  }
  // Ex: skeleton as "currency/TWD .00 rounding-mode-half-up"
  return ReadOnlyRoots(isolate).symbol_string_handle();
}

Handle<Object> UseGroupingFromSkeleton(Isolate* isolate,
                                       const icu::UnicodeString& skeleton) {
  Factory* factory = isolate->factory();
  static const char* group = "group-";
  int32_t start = skeleton.indexOf(group);
  if (start >= 0) {
    DCHECK_EQ(6, strlen(group));
    icu::UnicodeString check = skeleton.tempSubString(start + 6);
    // Ex: skeleton as
    // .### rounding-mode-half-up group-off
    if (check.startsWith("off")) {
      return factory->false_value();
    }
    // Ex: skeleton as
    // .### rounding-mode-half-up group-min2
    if (check.startsWith("min2")) {
      return ReadOnlyRoots(isolate).min2_string_handle();
    }
    // Ex: skeleton as
    // .### rounding-mode-half-up group-on-aligned
    if (check.startsWith("on-aligned")) {
      return ReadOnlyRoots(isolate).always_string_handle();
    }
  }
  // Ex: skeleton as
  // .###
  return ReadOnlyRoots(isolate).auto_string_handle();
}

// Parse currency code from skeleton. For example, skeleton as
// "currency/TWD .00 rounding-mode-half-up unit-width-full-name;"
const icu::UnicodeString CurrencyFromSkeleton(
    const icu::UnicodeString& skeleton) {
  const char currency[] = "currency/";
  int32_t index = skeleton.indexOf(currency);
  if (index < 0) return "";
  index += static_cast<int32_t>(std::strlen(currency));
  return skeleton.tempSubString(index, 3);
}

}  // namespace
const icu::UnicodeString JSNumberFormat::NumberingSystemFromSkeleton(
    const icu::UnicodeString& skeleton) {
  const char numbering_system[] = "numbering-system/";
  int32_t index = skeleton.indexOf(numbering_system);
  if (index < 0) return "latn";
  index += static_cast<int32_t>(std::strlen(numbering_system));
  const icu::UnicodeString res = skeleton.tempSubString(index);
  index = res.indexOf(" ");
  if (index < 0) return res;
  return res.tempSubString(0, index);
}

namespace {

// Return CurrencySign as string based on skeleton.
Handle<String> CurrencySignString(Isolate* isolate,
                                  const icu::UnicodeString& skeleton) {
  // Ex: skeleton as
  // "currency/TWD .00 rounding-mode-half-up sign-accounting-always" OR
  // "currency/TWD .00 rounding-mode-half-up sign-accounting-except-zero"
  if (skeleton.indexOf("sign-accounting") >= 0) {
    return ReadOnlyRoots(isolate).accounting_string_handle();
  }
  return ReadOnlyRoots(isolate).standard_string_handle();
}

// Return UnitDisplay as string based on skeleton.
Handle<String> UnitDisplayString(Isolate* isolate,
                                 const icu::UnicodeString& skeleton) {
  // Ex: skeleton as
  // "unit/length-meter .### rounding-mode-half-up unit-width-full-name"
  if (skeleton.indexOf("unit-width-full-name") >= 0) {
    return ReadOnlyRoots(isolate).long_string_handle();
  }
  // Ex: skeleton as
  // "unit/length-meter .### rounding-mode-half-up unit-width-narrow".
  if (skeleton.indexOf("unit-width-narrow") >= 0) {
    return ReadOnlyRoots(isolate).narrow_string_handle();
  }
  // Ex: skeleton as
  // "unit/length-foot .### rounding-mode-half-up"
  return ReadOnlyRoots(isolate).short_string_handle();
}

// Parse Notation from skeleton.
Notation NotationFromSkeleton(const icu::UnicodeString& skeleton) {
  // Ex: skeleton as
  // "scientific .### rounding-mode-half-up"
  if (skeleton.indexOf("scientific") >= 0) {
    return Notation::SCIENTIFIC;
  }
  // Ex: skeleton as
  // "engineering .### rounding-mode-half-up"
  if (skeleton.indexOf("engineering") >= 0) {
    return Notation::ENGINEERING;
  }
  // Ex: skeleton as
  // "compact-short .### rounding-mode-half-up" or
  // "compact-long .### rounding-mode-half-up
  if (skeleton.indexOf("compact-") >= 0) {
    return Notation::COMPACT;
  }
  // Ex: skeleton as
  // "unit/length-foot .### rounding-mode-half-up"
  return Notation::STANDARD;
}

Handle<String> NotationAsString(Isolate* isolate, Notation notation) {
  switch (notation) {
    case Notation::SCIENTIFIC:
      return ReadOnlyRoots(isolate).scientific_string_handle();
    case Notation::ENGINEERING:
      return ReadOnlyRoots(isolate).engineering_string_handle();
    case Notation::COMPACT:
      return ReadOnlyRoots(isolate).compact_string_handle();
    case Notation::STANDARD:
      return ReadOnlyRoots(isolate).standard_string_handle();
  }
  UNREACHABLE();
}

// Return CompactString as string based on skeleton.
Handle<String> CompactDisplayString(Isolate* isolate,
                                    const icu::UnicodeString& skeleton) {
  // Ex: skeleton as
  // "compact-long .### rounding-mode-half-up"
  if (skeleton.indexOf("compact-long") >= 0) {
    return ReadOnlyRoots(isolate).long_string_handle();
  }
  // Ex: skeleton as
  // "compact-short .### rounding-mode-half-up"
  DCHECK_GE(skeleton.indexOf("compact-short"), 0);
  return ReadOnlyRoots(isolate).short_string_handle();
}

// Return SignDisplay as string based on skeleton.
Handle<String> SignDisplayString(Isolate* isolate,
                                 const icu::UnicodeString& skeleton) {
  // Ex: skeleton as
  // "currency/TWD .00 rounding-mode-half-up sign-never"
  if (skeleton.indexOf("sign-never") >= 0) {
    return ReadOnlyRoots(isolate).never_string_handle();
  }
  // Ex: skeleton as
  // ".### rounding-mode-half-up sign-always" or
  // "currency/TWD .00 rounding-mode-half-up sign-accounting-always"
  if (skeleton.indexOf("sign-always") >= 0 ||
      skeleton.indexOf("sign-accounting-always") >= 0) {
    return ReadOnlyRoots(isolate).always_string_handle();
  }
  // Ex: skeleton as
  // "currency/TWD .00 rounding-mode-half-up sign-accounting-except-zero" or
  // "currency/TWD .00 rounding-mode-half-up sign-except-zero"
  if (skeleton.indexOf("sign-accounting-except-zero") >= 0 ||
      skeleton.indexOf("sign-except-zero") >= 0) {
    return ReadOnlyRoots(isolate).exceptZero_string_handle();
  }
  // Ex: skeleton as
  // ".### rounding-mode-half-up sign-negative" or
  // "currency/TWD .00 rounding-mode-half-up sign-accounting-negative"
  if (skeleton.indexOf("sign-accounting-negative") >= 0 ||
      skeleton.indexOf("sign-negative") >= 0) {
    return ReadOnlyRoots(isolate).negative_string_handle();
  }
  return ReadOnlyRoots(isolate).auto_string_handle();
}

// Return RoundingMode as string based on skeleton.
Handle<String> RoundingModeString(Isolate* isolate,
                                  const icu::UnicodeString& skeleton) {
  static const char* rounding_mode = "rounding-mode-";
  int32_t start = skeleton.indexOf(rounding_mode);
  if (start >= 0) {
    DCHECK_EQ(14, strlen(rounding_mode));
    icu::UnicodeString check = skeleton.tempSubString(start + 14);

    // Ex: skeleton as
    // .### rounding-mode-ceiling
    if (check.startsWith("ceiling")) {
      return ReadOnlyRoots(isolate).ceil_string_handle();
    }
    // Ex: skeleton as
    // .### rounding-mode-down
    if (check.startsWith("down")) {
      return ReadOnlyRoots(isolate).trunc_string_handle();
    }
    // Ex: skeleton as
    // .### rounding-mode-floor
    if (check.startsWith("floor")) {
      return ReadOnlyRoots(isolate).floor_string_handle();
    }
    // Ex: skeleton as
    // .### rounding-mode-half-ceiling
    if (check.startsWith("half-ceiling")) {
      return ReadOnlyRoots(isolate).halfCeil_string_handle();
    }
    // Ex: skeleton as
    // .### rounding-mode-half-down
    if (check.startsWith("half-down")) {
      return ReadOnlyRoots(isolate).halfTrunc_string_handle();
    }
    // Ex: skeleton as
    // .### rounding-mode-half-floor
    if (check.startsWith("half-floor")) {
      return ReadOnlyRoots(isolate).halfFloor_string_handle();
    }
    // Ex: skeleton as
    // .### rounding-mode-half-up
    if (check.startsWith("half-up")) {
      return ReadOnlyRoots(isolate).halfExpand_string_handle();
    }
    // Ex: skeleton as
    // .### rounding-mode-up
    if (check.startsWith("up")) {
      return ReadOnlyRoots(isolate).expand_string_handle();
    }
  }
  // Ex: skeleton as
  // .###
  return ReadOnlyRoots(isolate).halfEven_string_handle();
}

Handle<Object> RoundingIncrement(Isolate* isolate,
                                 const icu::UnicodeString& skeleton) {
  int32_t cur = skeleton.indexOf(u"precision-increment/");
  if (cur < 0) return isolate->factory()->NewNumberFromInt(1);
  cur += 20;  // length of "precision-increment/"
  int32_t increment = 0;
  while (cur < skeleton.length()) {
    char16_t c = skeleton[cur++];
    if (c == u'.') continue;
    if (!IsDecimalDigit(c)) break;
    increment = increment * 10 + (c - '0');
  }
  return isolate->factory()->NewNumberFromInt(increment);
}

// Return RoundingPriority as string based on skeleton.
Handle<String> RoundingPriorityString(Isolate* isolate,
                                      const icu::UnicodeString& skeleton) {
  int32_t found;
  // If #r or @r is followed by a SPACE or in the end of line.
  if ((found = skeleton.indexOf("#r")) >= 0 ||
      (found = skeleton.indexOf("@r")) >= 0) {
    if (found + 2 == skeleton.length() || skeleton[found + 2] == ' ') {
      return ReadOnlyRoots(isolate).morePrecision_string_handle();
    }
  }
  // If #s or @s is followed by a SPACE or in the end of line.
  if ((found = skeleton.indexOf("#s")) >= 0 ||
      (found = skeleton.indexOf("@s")) >= 0) {
    if (found + 2 == skeleton.length() || skeleton[found + 2] == ' ') {
      return ReadOnlyRoots(isolate).lessPrecision_string_handle();
    }
  }
  return ReadOnlyRoots(isolate).auto_string_handle();
}

// Return trailingZeroDisplay as string based on skeleton.
Handle<String> TrailingZeroDisplayString(Isolate* isolate,
                                         const icu::UnicodeString& skeleton) {
  int32_t found;
  if ((found = skeleton.indexOf("/w")) >= 0) {
    if (found + 2 == skeleton.length() || skeleton[found + 2] == ' ') {
      return ReadOnlyRoots(isolate).stripIfInteger_string_handle();
    }
  }
  return ReadOnlyRoots(isolate).auto_string_handle();
}

}  // anonymous namespace

// Return the minimum integer digits by counting the number of '0' after
// "integer-width/*" in the skeleton.
// Ex: Return 15 for skeleton as
// “currency/TWD .00 rounding-mode-half-up integer-width/*000000000000000”
//                                                                 1
//                                                        123456789012345
// Return default value as 1 if there are no "integer-width/*".
int32_t JSNumberFormat::MinimumIntegerDigitsFromSkeleton(
    const icu::UnicodeString& skeleton) {
  // count the number of 0 after "integer-width/*"
  icu::UnicodeString search("integer-width/*");
  int32_t index = skeleton.indexOf(search);
  if (index < 0) return 1;  // return 1 if cannot find it.
  index += search.length();
  int32_t matched = 0;
  while (index < skeleton.length() && skeleton[index] == '0') {
    matched++;
    index++;
  }
  DCHECK_GT(matched, 0);
  return matched;
}

// Return true if there are fraction digits, false if not.
// The minimum fraction digits is the number of '0' after '.' in the skeleton
// The maximum fraction digits is the number of '#' after the above '0's plus
// the minimum fraction digits.
// For example, as skeleton “.000#### rounding-mode-half-up”
//                            123
//                               4567
// Set The minimum as 3 and maximum as 7.
// We also treat the following  special cases as both minimum and maximum are 0
// while there are no . in the skeleton:
// 1. While there are "precision-integer" in the skeleton.
// 2. While there are "precision-increment/" in the skeleton but no . after it.
// Examples:
// "currency/JPY precision-integer rounding-mode-half-up"
// "precision-increment/2 rounding-mode-half-up"
bool JSNumberFormat::FractionDigitsFromSkeleton(
    const icu::UnicodeString& skeleton, int32_t* minimum, int32_t* maximum) {
  int32_t index = skeleton.indexOf(".");
  if (index < 0) {
    // https://unicode-org.github.io/icu/userguide/format_parse/numbers/skeletons.html#precision
    // Note that the stem . is considered valid and is equivalent to
    // precision-integer.
    // Also, if there are "precision-increment/" but no "." we consider both
    // minimum and maximum fraction digits as 0.
    if (skeleton.indexOf("precision-integer") >= 0 ||
        skeleton.indexOf("precision-increment/") >= 0) {
      *minimum = *maximum = 0;
      return true;
    }
    return false;
  }
  *minimum = 0;
  index++;  // skip the '.'
  while (index < skeleton.length() && IsDecimalDigit(skeleton[index])) {
    (*minimum)++;
    index++;
  }
  *maximum = *minimum;
  while (index < skeleton.length() && skeleton[index] == '#') {
    (*maximum)++;
    index++;
  }
  return true;
}

// Return true if there are significant digits, false if not.
// The minimum significant digits is the number of '@' in the skeleton
// The maximum significant digits is the number of '#' after these '@'s plus
// the minimum significant digits.
// Ex: Skeleton as "@@@@@####### rounding-mode-half-up"
//                  12345
//                       6789012
// Set The minimum as 5 and maximum as 12.
bool JSNumberFormat::SignificantDigitsFromSkeleton(
    const icu::UnicodeString& skeleton, int32_t* minimum, int32_t* maximum) {
  int32_t index = skeleton.indexOf("@");
  if (index < 0) return false;
  *minimum = 1;
  index++;  // skip the first '@'
  while (index < skeleton.length() && skeleton[index] == '@') {
    (*minimum)++;
    index++;
  }
  *maximum = *minimum;
  while (index < skeleton.length() && skeleton[index] == '#') {
    (*maximum)++;
    index++;
  }
  return true;
}

namespace {

// Ex: percent .### rounding-mode-half-up
// Special case for "percent"
// Ex: "unit/milliliter-per-acre .### rounding-mode-half-up"
// should return "milliliter-per-acre".
// Ex: "unit/year .### rounding-mode-half-up" should return
// "year".
std::string UnitFromSkeleton(const icu::UnicodeString& skeleton) {
  std::string str;
  str = skeleton.toUTF8String<std::string>(str);
  std::string search("unit/");
  size_t begin = str.find(search);
  if (begin == str.npos) {
    // Special case for "percent".
    if (str.find("percent") != str.npos) {
      return "percent";
    }
    return "";
  }
  // Ex:
  // "unit/acre .### rounding-mode-half-up"
  //       b
  // Ex:
  // "unit/milliliter-per-acre .### rounding-mode-half-up"
  //       b
  begin += search.size();
  if (begin == str.npos) {
    return "";
  }
  // Find the end of the subtype.
  size_t end = str.find(" ", begin);
  // Ex:
  // "unit/acre .### rounding-mode-half-up"
  //       b   e
  // Ex:
  // "unit/milliliter-per-acre .### rounding-mode-half-up"
  //       b                  e
  if (end == str.npos) {
    end = str.size();
  }
  return str.substr(begin, end - begin);
}

Style StyleFromSkeleton(const icu::UnicodeString& skeleton) {
  if (skeleton.indexOf("currency/") >= 0) {
    return Style::CURRENCY;
  }
  if (skeleton.indexOf("percent") >= 0) {
    // percent precision-integer rounding-mode-half-up scale/100
    if (skeleton.indexOf("scale/100") >= 0) {
      return Style::PERCENT;
    } else {
      return Style::UNIT;
    }
  }
  // Before ICU68: "measure-unit/", since ICU68 "unit/"
  if (skeleton.indexOf("unit/") >= 0) {
    return Style::UNIT;
  }
  return Style::DECIMAL;
}

}  // anonymous namespace

icu::number::UnlocalizedNumberFormatter
JSNumberFormat::SetDigitOptionsToFormatter(
    const icu::number::UnlocalizedNumberFormatter& settings,
    const Intl::NumberFormatDigitOptions& digit_options, int rounding_increment,
    JSNumberFormat::ShowTrailingZeros trailing_zeros) {
  icu::number::UnlocalizedNumberFormatter result = settings;
  if (digit_options.minimum_integer_digits > 1) {
    result = result.integerWidth(icu::number::IntegerWidth::zeroFillTo(
        digit_options.minimum_integer_digits));
  }

  icu::number::Precision precision = icu::number::Precision::unlimited();
  bool relaxed = false;
  switch (digit_options.rounding_type) {
    case Intl::RoundingType::kSignificantDigits:
      precision = icu::number::Precision::minMaxSignificantDigits(
          digit_options.minimum_significant_digits,
          digit_options.maximum_significant_digits);
      break;
    case Intl::RoundingType::kFractionDigits:
      precision = icu::number::Precision::minMaxFraction(
          digit_options.minimum_fraction_digits,
          digit_options.maximum_fraction_digits);
      break;
    case Intl::RoundingType::kMorePrecision:
      relaxed = true;
      V8_FALLTHROUGH;
    case Intl::RoundingType::kLessPrecision:
      precision =
          icu::number::Precision::minMaxFraction(
              digit_options.minimum_fraction_digits,
              digit_options.maximum_fraction_digits)
              .withSignificantDigits(digit_options.minimum_significant_digits,
                                     digit_options.maximum_significant_digits,
                                     relaxed ? UNUM_ROUNDING_PRIORITY_RELAXED
                                             : UNUM_ROUNDING_PRIORITY_STRICT);
      break;
  }
  if (rounding_increment != 1) {
    precision = ::icu::number::Precision::incrementExact(
                    rounding_increment, -digit_options.maximum_fraction_digits)
                    .withMinFraction(digit_options.minimum_fraction_digits);
  }
  if (trailing_zeros == JSNumberFormat::ShowTrailingZeros::kHide) {
    precision = precision.trailingZeroDisplay(UNUM_TRAILING_ZERO_HIDE_IF_WHOLE);
  }
  return result.precision(precision);
}

// static
// ecma402 #sec-intl.numberformat.prototype.resolvedoptions
Handle<JSObject> JSNumberFormat::ResolvedOptions(
    Isolate* isolate, Handle<JSNumberFormat> number_format) {
  Factory* factory = isolate->factory();

  UErrorCode status = U_ZERO_ERROR;
  icu::number::LocalizedNumberFormatter* fmt =
      number_format->icu_number_formatter().raw();
  icu::UnicodeString skeleton = fmt->toSkeleton(status);
  DCHECK(U_SUCCESS(status));

  // 4. Let options be ! ObjectCreate(%ObjectPrototype%).
  Handle<JSObject> options = factory->NewJSObject(isolate->object_function());

  Handle<String> locale = Handle<String>(number_format->locale(), isolate);
  const icu::UnicodeString numberingSystem_ustr =
      JSNumberFormat::NumberingSystemFromSkeleton(skeleton);
  // 5. For each row of Table 4, except the header row, in table order, do
  // Table 4: Resolved Options of NumberFormat Instances
  //  Internal Slot                    Property
  //    [[Locale]]                      "locale"
  //    [[NumberingSystem]]             "numberingSystem"
  //    [[Style]]                       "style"
  //    [[Currency]]                    "currency"
  //    [[CurrencyDisplay]]             "currencyDisplay"
  //    [[CurrencySign]]                "currencySign"
  //    [[Unit]]                        "unit"
  //    [[UnitDisplay]]                 "unitDisplay"
  //    [[MinimumIntegerDigits]]        "minimumIntegerDigits"
  //    [[MinimumFractionDigits]]       "minimumFractionDigits"
  //    [[MaximumFractionDigits]]       "maximumFractionDigits"
  //    [[MinimumSignificantDigits]]    "minimumSignificantDigits"
  //    [[MaximumSignificantDigits]]    "maximumSignificantDigits"
  //    [[UseGrouping]]                 "useGrouping"
  //    [[Notation]]                    "notation"
  //    [[CompactDisplay]]              "compactDisplay"
  //    [[SignDisplay]]                 "signDisplay"
  //    [[RoundingMode]]                "roundingMode"
  //    [[RoundingIncrement]]           "roundingIncrement"
  //    [[TrailingZeroDisplay]]         "trailingZeroDisplay"

  CHECK(JSReceiver::CreateDataProperty(isolate, options,
                                       factory->locale_string(), locale,
                                       Just(kDontThrow))
            .FromJust());
  Handle<String> numberingSystem_string;
  CHECK(Intl::ToString(isolate, numberingSystem_ustr)
            .ToHandle(&numberingSystem_string));
  CHECK(JSReceiver::CreateDataProperty(isolate, options,
                                       factory->numberingSystem_string(),
                                       numberingSystem_string, Just(kDontThrow))
            .FromJust());
  Style style = StyleFromSkeleton(skeleton);
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->style_string(),
            StyleAsString(isolate, style), Just(kDontThrow))
            .FromJust());
  const icu::UnicodeString currency_ustr = CurrencyFromSkeleton(skeleton);
  if (!currency_ustr.isEmpty()) {
    Handle<String> currency_string;
    CHECK(Intl::ToString(isolate, currency_ustr).ToHandle(&currency_string));
    CHECK(JSReceiver::CreateDataProperty(isolate, options,
                                         factory->currency_string(),
                                         currency_string, Just(kDontThrow))
              .FromJust());

    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->currencyDisplay_string(),
              CurrencyDisplayString(isolate, skeleton), Just(kDontThrow))
              .FromJust());
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->currencySign_string(),
              CurrencySignString(isolate, skeleton), Just(kDontThrow))
              .FromJust());
  }

  if (style == Style::UNIT) {
    std::string unit = UnitFromSkeleton(skeleton);
    if (!unit.empty()) {
      CHECK(JSReceiver::CreateDataProperty(
                isolate, options, factory->unit_string(),
                isolate->factory()->NewStringFromAsciiChecked(unit.c_str()),
                Just(kDontThrow))
                .FromJust());
    }
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->unitDisplay_string(),
              UnitDisplayString(isolate, skeleton), Just(kDontThrow))
              .FromJust());
  }

  CHECK(
      JSReceiver::CreateDataProperty(
          isolate, options, factory->minimumIntegerDigits_string(),
          factory->NewNumberFromInt(MinimumIntegerDigitsFromSkeleton(skeleton)),
          Just(kDontThrow))
          .FromJust());

  int32_t mnsd = 0, mxsd = 0, mnfd = 0, mxfd = 0;
  bool has_significant_digits =
      SignificantDigitsFromSkeleton(skeleton, &mnsd, &mxsd);
  if (has_significant_digits) {
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->minimumSignificantDigits_string(),
              factory->NewNumberFromInt(mnsd), Just(kDontThrow))
              .FromJust());
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->maximumSignificantDigits_string(),
              factory->NewNumberFromInt(mxsd), Just(kDontThrow))
              .FromJust());
  }
  if (FractionDigitsFromSkeleton(skeleton, &mnfd, &mxfd)) {
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->minimumFractionDigits_string(),
              factory->NewNumberFromInt(mnfd), Just(kDontThrow))
              .FromJust());
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->maximumFractionDigits_string(),
              factory->NewNumberFromInt(mxfd), Just(kDontThrow))
              .FromJust());
  }

  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->useGrouping_string(),
            UseGroupingFromSkeleton(isolate, skeleton), Just(kDontThrow))
            .FromJust());

  Notation notation = NotationFromSkeleton(skeleton);
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->notation_string(),
            NotationAsString(isolate, notation), Just(kDontThrow))
            .FromJust());
  // Only output compactDisplay when notation is compact.
  if (notation == Notation::COMPACT) {
    CHECK(JSReceiver::CreateDataProperty(
              isolate, options, factory->compactDisplay_string(),
              CompactDisplayString(isolate, skeleton), Just(kDontThrow))
              .FromJust());
  }
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->signDisplay_string(),
            SignDisplayString(isolate, skeleton), Just(kDontThrow))
            .FromJust());
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->roundingMode_string(),
            RoundingModeString(isolate, skeleton), Just(kDontThrow))
            .FromJust());
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->roundingIncrement_string(),
            RoundingIncrement(isolate, skeleton), Just(kDontThrow))
            .FromJust());
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->trailingZeroDisplay_string(),
            TrailingZeroDisplayString(isolate, skeleton), Just(kDontThrow))
            .FromJust());
  CHECK(JSReceiver::CreateDataProperty(
            isolate, options, factory->roundingPriority_string(),
            RoundingPriorityString(isolate, skeleton), Just(kDontThrow))
            .FromJust());
  return options;
}

// ecma402/#sec-unwrapnumberformat
MaybeHandle<JSNumberFormat> JSNumberFormat::UnwrapNumberFormat(
    Isolate* isolate, Handle<JSReceiver> format_holder) {
  // old code copy from NumberFormat::Unwrap that has no spec comment and
  // compiled but fail unit tests.
  Handle<Context> native_context =
      Handle<Context>(isolate->context().native_context(), isolate);
  Handle<JSFunction> constructor = Handle<JSFunction>(
      JSFunction::cast(native_context->intl_number_format_function()), isolate);
  Handle<Object> object;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, object,
      Intl::LegacyUnwrapReceiver(isolate, format_holder, constructor,
                                 format_holder->IsJSNumberFormat()),
      JSNumberFormat);
  // 4. If ... or nf does not have an [[InitializedNumberFormat]] internal slot,
  // then
  if (!object->IsJSNumberFormat()) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kIncompatibleMethodReceiver,
                                 isolate->factory()->NewStringFromAsciiChecked(
                                     "UnwrapNumberFormat")),
                    JSNumberFormat);
  }
  // 5. Return nf.
  return Handle<JSNumberFormat>::cast(object);
}

// 22. is in « 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500,
// 5000 »
bool IsValidRoundingIncrement(int value) {
  return value == 1 || value == 2 || value == 5 || value == 10 || value == 20 ||
         value == 25 || value == 50 || value == 100 || value == 200 ||
         value == 250 || value == 500 || value == 1000 || value == 2000 ||
         value == 2500 || value == 5000;
}
// static
MaybeHandle<JSNumberFormat> JSNumberFormat::New(Isolate* isolate,
                                                Handle<Map> map,
                                                Handle<Object> locales,
                                                Handle<Object> options_obj,
                                                const char* service) {
  Factory* factory = isolate->factory();

  // 1. Let requestedLocales be ? CanonicalizeLocaleList(locales).
  Maybe<std::vector<std::string>> maybe_requested_locales =
      Intl::CanonicalizeLocaleList(isolate, locales);
  MAYBE_RETURN(maybe_requested_locales, Handle<JSNumberFormat>());
  std::vector<std::string> requested_locales =
      maybe_requested_locales.FromJust();

  // 2. Set options to ? CoerceOptionsToObject(options).
  Handle<JSReceiver> options;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, options, CoerceOptionsToObject(isolate, options_obj, service),
      JSNumberFormat);

  // 3. Let opt be a new Record.
  // 4. Let matcher be ? GetOption(options, "localeMatcher", "string", «
  // "lookup", "best fit" », "best fit").
  // 5. Set opt.[[localeMatcher]] to matcher.
  Maybe<Intl::MatcherOption> maybe_locale_matcher =
      Intl::GetLocaleMatcher(isolate, options, service);
  MAYBE_RETURN(maybe_locale_matcher, MaybeHandle<JSNumberFormat>());
  Intl::MatcherOption matcher = maybe_locale_matcher.FromJust();

  std::unique_ptr<char[]> numbering_system_str = nullptr;
  // 6. Let _numberingSystem_ be ? GetOption(_options_, `"numberingSystem"`,
  //    `"string"`, *undefined*, *undefined*).
  Maybe<bool> maybe_numberingSystem = Intl::GetNumberingSystem(
      isolate, options, service, &numbering_system_str);
  // 7. If _numberingSystem_ is not *undefined*, then
  // 8. If _numberingSystem_ does not match the
  //    `(3*8alphanum) *("-" (3*8alphanum))` sequence, throw a *RangeError*
  //     exception.
  MAYBE_RETURN(maybe_numberingSystem, MaybeHandle<JSNumberFormat>());

  // 9. Let localeData be %NumberFormat%.[[LocaleData]].
  // 10. Let r be ResolveLocale(%NumberFormat%.[[AvailableLocales]],
  // requestedLocales, opt,  %NumberFormat%.[[RelevantExtensionKeys]],
  // localeData).
  std::set<std::string> relevant_extension_keys{"nu"};
  Maybe<Intl::ResolvedLocale> maybe_resolve_locale =
      Intl::ResolveLocale(isolate, JSNumberFormat::GetAvailableLocales(),
                          requested_locales, matcher, relevant_extension_keys);
  if (maybe_resolve_locale.IsNothing()) {
    THROW_NEW_ERROR(isolate, NewRangeError(MessageTemplate::kIcuError),
                    JSNumberFormat);
  }
  Intl::ResolvedLocale r = maybe_resolve_locale.FromJust();

  icu::Locale icu_locale = r.icu_locale;
  UErrorCode status = U_ZERO_ERROR;
  if (numbering_system_str != nullptr) {
    auto nu_extension_it = r.extensions.find("nu");
    if (nu_extension_it != r.extensions.end() &&
        nu_extension_it->second != numbering_system_str.get()) {
      icu_locale.setUnicodeKeywordValue("nu", nullptr, status);
      DCHECK(U_SUCCESS(status));
    }
  }

  // 9. Set numberFormat.[[Locale]] to r.[[locale]].
  Maybe<std::string> maybe_locale_str = Intl::ToLanguageTag(icu_locale);
  MAYBE_RETURN(maybe_locale_str, MaybeHandle<JSNumberFormat>());
  Handle<String> locale_str = isolate->factory()->NewStringFromAsciiChecked(
      maybe_locale_str.FromJust().c_str());

  if (numbering_system_str != nullptr &&
      Intl::IsValidNumberingSystem(numbering_system_str.get())) {
    icu_locale.setUnicodeKeywordValue("nu", numbering_system_str.get(), status);
    DCHECK(U_SUCCESS(status));
  }

  std::string numbering_system = Intl::GetNumberingSystem(icu_locale);

  // 11. Let dataLocale be r.[[dataLocale]].

  icu::number::UnlocalizedNumberFormatter settings =
      icu::number::UnlocalizedNumberFormatter().roundingMode(UNUM_ROUND_HALFUP);

  // For 'latn' numbering system, skip the adoptSymbols which would cause
  // 10.1%-13.7% of regression of JSTests/Intl-NewIntlNumberFormat
  // See crbug/1052751 so we skip calling adoptSymbols and depending on the
  // default instead.
  if (!numbering_system.empty() && numbering_system != "latn") {
    settings = settings.adoptSymbols(icu::NumberingSystem::createInstanceByName(
        numbering_system.c_str(), status));
    DCHECK(U_SUCCESS(status));
  }

  // ==== Start SetNumberFormatUnitOptions ====
  // 3. Let style be ? GetOption(options, "style", "string",  « "decimal",
  // "percent", "currency", "unit" », "decimal").

  Maybe<Style> maybe_style = GetStringOption<Style>(
      isolate, options, "style", service,
      {"decimal", "percent", "currency", "unit"},
      {Style::DECIMAL, Style::PERCENT, Style::CURRENCY, Style::UNIT},
      Style::DECIMAL);
  MAYBE_RETURN(maybe_style, MaybeHandle<JSNumberFormat>());
  Style style = maybe_style.FromJust();

  // 4. Set intlObj.[[Style]] to style.

  // 5. Let currency be ? GetOption(options, "currency", "string", undefined,
  // undefined).
  std::unique_ptr<char[]> currency_cstr;
  const std::vector<const char*> empty_values = {};
  Maybe<bool> found_currency = GetStringOption(
      isolate, options, "currency", empty_values, service, &currency_cstr);
  MAYBE_RETURN(found_currency, MaybeHandle<JSNumberFormat>());

  std::string currency;
  // 6. If currency is not undefined, then
  if (found_currency.FromJust()) {
    DCHECK_NOT_NULL(currency_cstr.get());
    currency = currency_cstr.get();
    // 6. a. If the result of IsWellFormedCurrencyCode(currency) is false,
    // throw a RangeError exception.
    if (!IsWellFormedCurrencyCode(currency)) {
      THROW_NEW_ERROR(
          isolate,
          NewRangeError(MessageTemplate::kInvalid,
                        factory->NewStringFromStaticChars("currency code"),
                        factory->NewStringFromAsciiChecked(currency.c_str())),
          JSNumberFormat);
    }
  } else {
    // 7. If style is "currency" and currency is undefined, throw a TypeError
    // exception.
    if (style == Style::CURRENCY) {
      THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kCurrencyCode),
                      JSNumberFormat);
    }
  }
  // 8. Let currencyDisplay be ? GetOption(options, "currencyDisplay",
  // "string", « "code",  "symbol", "name", "narrowSymbol" », "symbol").
  Maybe<CurrencyDisplay> maybe_currency_display =
      GetStringOption<CurrencyDisplay>(
          isolate, options, "currencyDisplay", service,
          {"code", "symbol", "name", "narrowSymbol"},
          {CurrencyDisplay::CODE, CurrencyDisplay::SYMBOL,
           CurrencyDisplay::NAME, CurrencyDisplay::NARROW_SYMBOL},
          CurrencyDisplay::SYMBOL);
  MAYBE_RETURN(maybe_currency_display, MaybeHandle<JSNumberFormat>());
  CurrencyDisplay currency_display = maybe_currency_display.FromJust();

  CurrencySign currency_sign = CurrencySign::STANDARD;
  // 9. Let currencySign be ? GetOption(options, "currencySign", "string", «
  // "standard",  "accounting" », "standard").
  Maybe<CurrencySign> maybe_currency_sign = GetStringOption<CurrencySign>(
      isolate, options, "currencySign", service, {"standard", "accounting"},
      {CurrencySign::STANDARD, CurrencySign::ACCOUNTING},
      CurrencySign::STANDARD);
  MAYBE_RETURN(maybe_currency_sign, MaybeHandle<JSNumberFormat>());
  currency_sign = maybe_currency_sign.FromJust();

  // 10. Let unit be ? GetOption(options, "unit", "string", undefined,
  // undefined).
  std::unique_ptr<char[]> unit_cstr;
  Maybe<bool> found_unit = GetStringOption(isolate, options, "unit",
                                           empty_values, service, &unit_cstr);
  MAYBE_RETURN(found_unit, MaybeHandle<JSNumberFormat>());

  std::pair<icu::MeasureUnit, icu::MeasureUnit> unit_pair;
  // 11. If unit is not undefined, then
  if (found_unit.FromJust()) {
    DCHECK_NOT_NULL(unit_cstr.get());
    std::string unit = unit_cstr.get();
    // 11.a If the result of IsWellFormedUnitIdentifier(unit) is false, throw a
    // RangeError exception.
    Maybe<std::pair<icu::MeasureUnit, icu::MeasureUnit>> maybe_wellformed_unit =
        IsWellFormedUnitIdentifier(isolate, unit);
    if (maybe_wellformed_unit.IsNothing()) {
      THROW_NEW_ERROR(
          isolate,
          NewRangeError(MessageTemplate::kInvalidUnit,
                        factory->NewStringFromAsciiChecked(service),
                        factory->NewStringFromAsciiChecked(unit.c_str())),
          JSNumberFormat);
    }
    unit_pair = maybe_wellformed_unit.FromJust();
  } else {
    // 12. If style is "unit" and unit is undefined, throw a TypeError
    // exception.
    if (style == Style::UNIT) {
      THROW_NEW_ERROR(isolate,
                      NewTypeError(MessageTemplate::kInvalidUnit,
                                   factory->NewStringFromAsciiChecked(service),
                                   factory->empty_string()),
                      JSNumberFormat);
    }
  }

  // 13. Let unitDisplay be ? GetOption(options, "unitDisplay", "string", «
  // "short", "narrow", "long" »,  "short").
  Maybe<UnitDisplay> maybe_unit_display = GetStringOption<UnitDisplay>(
      isolate, options, "unitDisplay", service, {"short", "narrow", "long"},
      {UnitDisplay::SHORT, UnitDisplay::NARROW, UnitDisplay::LONG},
      UnitDisplay::SHORT);
  MAYBE_RETURN(maybe_unit_display, MaybeHandle<JSNumberFormat>());
  UnitDisplay unit_display = maybe_unit_display.FromJust();

  // 14. If style is "currency", then
  icu::UnicodeString currency_ustr;
  if (style == Style::CURRENCY) {
    // 14.a. If currency is undefined, throw a TypeError exception.
    if (!found_currency.FromJust()) {
      THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kCurrencyCode),
                      JSNumberFormat);
    }
    // 14.a. Let currency be the result of converting currency to upper case as
    //    specified in 6.1
    std::transform(currency.begin(), currency.end(), currency.begin(), toupper);
    currency_ustr = currency.c_str();

    // 14.b. Set numberFormat.[[Currency]] to currency.
    if (!currency_ustr.isEmpty()) {
      Handle<String> currency_string;
      ASSIGN_RETURN_ON_EXCEPTION(isolate, currency_string,
                                 Intl::ToString(isolate, currency_ustr),
                                 JSNumberFormat);

      settings =
          settings.unit(icu::CurrencyUnit(currency_ustr.getBuffer(), status));
      DCHECK(U_SUCCESS(status));
      // 14.c Set intlObj.[[CurrencyDisplay]] to currencyDisplay.
      // The default unitWidth is SHORT in ICU and that mapped from
      // Symbol so we can skip the setting for optimization.
      if (currency_display != CurrencyDisplay::SYMBOL) {
        settings = settings.unitWidth(ToUNumberUnitWidth(currency_display));
      }
      DCHECK(U_SUCCESS(status));
    }
  }

  // 15. If style is "unit", then
  if (style == Style::UNIT) {
    // Track newer style "unit".
    isolate->CountUsage(v8::Isolate::UseCounterFeature::kNumberFormatStyleUnit);

    icu::MeasureUnit none = icu::MeasureUnit();
    // 13.b Set intlObj.[[Unit]] to unit.
    if (unit_pair.first != AVOID_AMBIGUOUS_OP_WARNING(none)) {
      settings = settings.unit(unit_pair.first);
    }
    if (unit_pair.second != AVOID_AMBIGUOUS_OP_WARNING(none)) {
      settings = settings.perUnit(unit_pair.second);
    }

    // The default unitWidth is SHORT in ICU and that mapped from
    // Symbol so we can skip the setting for optimization.
    if (unit_display != UnitDisplay::SHORT) {
      settings = settings.unitWidth(ToUNumberUnitWidth(unit_display));
    }
  }

  // === End of SetNumberFormatUnitOptions

  if (style == Style::PERCENT) {
    settings = settings.unit(icu::MeasureUnit::getPercent())
                   .scale(icu::number::Scale::powerOfTen(2));
  }

  // 16. If style is "currency", then
  int mnfd_default, mxfd_default;
  if (style == Style::CURRENCY) {
    // b. Let cDigits be CurrencyDigits(currency).
    int c_digits = CurrencyDigits(currency_ustr);
    // c. Let mnfdDefault be cDigits.
    // d. Let mxfdDefault be cDigits.
    mnfd_default = c_digits;
    mxfd_default = c_digits;
    // 17. Else,
  } else {
    // a. Let mnfdDefault be 0.
    mnfd_default = 0;
    // b. If style is "percent", then
    if (style == Style::PERCENT) {
      // i. Let mxfdDefault be 0.
      mxfd_default = 0;
    } else {
      // c. Else,
      // i. Let mxfdDefault be 3.
      mxfd_default = 3;
    }
  }

  int rounding_increment = 1;
  // 18. Let roundingIncrement be ? GetNumberOption(options,
  // "roundingIncrement,", 1, 5000, 1).
  Maybe<int> maybe_rounding_increment = GetNumberOption(
      isolate, options, factory->roundingIncrement_string(), 1, 5000, 1);
  if (!maybe_rounding_increment.To(&rounding_increment)) {
    return MaybeHandle<JSNumberFormat>();
  }

  // 19. If roundingIncrement is not in « 1, 2, 5, 10, 20, 25, 50, 100, 200,
  // 250, 500, 1000, 2000, 2500, 5000 », throw a RangeError exception.
  if (!IsValidRoundingIncrement(rounding_increment)) {
    THROW_NEW_ERROR(isolate,
                    NewRangeError(MessageTemplate::kPropertyValueOutOfRange,
                                  factory->roundingIncrement_string()),
                    JSNumberFormat);
  }
  // 20. If roundingIncrement is not 1, set mxfdDefault to mnfdDefault.
  if (rounding_increment != 1) {
    mxfd_default = mnfd_default;
  }

  Notation notation = Notation::STANDARD;
  // 21. Let notation be ? GetOption(options, "notation", "string", «
  // "standard", "scientific",  "engineering", "compact" », "standard").
  MAYBE_ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, notation,
      GetStringOption<Notation>(
          isolate, options, "notation", service,
          {"standard", "scientific", "engineering", "compact"},
          {Notation::STANDARD, Notation::SCIENTIFIC, Notation::ENGINEERING,
           Notation::COMPACT},
          Notation::STANDARD),
      Handle<JSNumberFormat>());
  // 22. Set numberFormat.[[Notation]] to notation.

  // 23. Perform ? SetNumberFormatDigitOptions(numberFormat, options,
  // mnfdDefault, mxfdDefault).
  Maybe<Intl::NumberFormatDigitOptions> maybe_digit_options =
      Intl::SetNumberFormatDigitOptions(isolate, options, mnfd_default,
                                        mxfd_default,
                                        notation == Notation::COMPACT);
  MAYBE_RETURN(maybe_digit_options, Handle<JSNumberFormat>());
  Intl::NumberFormatDigitOptions digit_options = maybe_digit_options.FromJust();

  // 24. If roundingIncrement is not 1, then
  if (rounding_increment != 1) {
    // a. If numberFormat.[[RoundingType]] is not fractionDigits, throw a
    // TypeError exception.
    if (digit_options.rounding_type != Intl::RoundingType::kFractionDigits) {
      THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kBadRoundingType),
                      JSNumberFormat);
    }
    // b. If numberFormat.[[MaximumFractionDigits]] is not equal to
    // numberFormat.[[MinimumFractionDigits]], throw a RangeError exception.
    if (digit_options.maximum_fraction_digits !=
        digit_options.minimum_fraction_digits) {
      THROW_NEW_ERROR(
          isolate,
          NewRangeError(
              MessageTemplate::
                  kMaximumFractionDigitsNotEqualMinimumFractionDigits),
          JSNumberFormat);
    }
  }

  // 25. Set numberFormat.[[RoundingIncrement]] to roundingIncrement.

  // 26. Let trailingZeroDisplay be ? GetOption(options,
  // "trailingZeroDisplay", "string", « "auto", "stripIfInteger" », "auto").
  Maybe<TrailingZeroDisplay> maybe_trailing_zero_display =
      GetStringOption<TrailingZeroDisplay>(
          isolate, options, "trailingZeroDisplay", service,
          {"auto", "stripIfInteger"},
          {TrailingZeroDisplay::AUTO, TrailingZeroDisplay::STRIP_IF_INTEGER},
          TrailingZeroDisplay::AUTO);
  MAYBE_RETURN(maybe_trailing_zero_display, MaybeHandle<JSNumberFormat>());
  TrailingZeroDisplay trailing_zero_display =
      maybe_trailing_zero_display.FromJust();

  // 27. Set numberFormat.[[TrailingZeroDisplay]] to trailingZeroDisplay.
  settings = SetDigitOptionsToFormatter(
      settings, digit_options, rounding_increment,
      trailing_zero_display == TrailingZeroDisplay::STRIP_IF_INTEGER
          ? ShowTrailingZeros::kHide
          : ShowTrailingZeros::kShow);

  // 28. Let compactDisplay be ? GetOption(options, "compactDisplay",
  // "string", « "short", "long" »,  "short").
  Maybe<CompactDisplay> maybe_compact_display = GetStringOption<CompactDisplay>(
      isolate, options, "compactDisplay", service, {"short", "long"},
      {CompactDisplay::SHORT, CompactDisplay::LONG}, CompactDisplay::SHORT);
  MAYBE_RETURN(maybe_compact_display, MaybeHandle<JSNumberFormat>());
  CompactDisplay compact_display = maybe_compact_display.FromJust();

  // The default notation in ICU is Simple, which mapped from STANDARD
  // so we can skip setting it.
  if (notation != Notation::STANDARD) {
    settings = settings.notation(ToICUNotation(notation, compact_display));
  }

  // 28. Let defaultUseGrouping be "auto".
  UseGrouping default_use_grouping = UseGrouping::AUTO;

  // 29. If notation is "compact", then
  if (notation == Notation::COMPACT) {
    // a. Set numberFormat.[[CompactDisplay]] to compactDisplay.
    // Done in above together
    // b. Set defaultUseGrouping to "min2".
    default_use_grouping = UseGrouping::MIN2;
  }

  // 30. Let useGrouping be ? GetStringOrBooleanOption(options, "useGrouping",
  // « "min2", "auto", "always" », "always", false, defaultUseGrouping).
  Maybe<UseGrouping> maybe_use_grouping = GetStringOrBooleanOption<UseGrouping>(
      isolate, options, "useGrouping", service, {"min2", "auto", "always"},
      {UseGrouping::MIN2, UseGrouping::AUTO, UseGrouping::ALWAYS},
      UseGrouping::ALWAYS,    // trueValue
      UseGrouping::OFF,       // falseValue
      default_use_grouping);  // fallbackValue
  MAYBE_RETURN(maybe_use_grouping, MaybeHandle<JSNumberFormat>());
  UseGrouping use_grouping = maybe_use_grouping.FromJust();
  // 31. Set numberFormat.[[UseGrouping]] to useGrouping.
  if (use_grouping != UseGrouping::AUTO) {
    settings = settings.grouping(ToUNumberGroupingStrategy(use_grouping));
  }

  // 32. Let signDisplay be ? GetOption(options, "signDisplay", "string", «
  // "auto", "never", "always",  "exceptZero", "negative" », "auto").
  Maybe<SignDisplay> maybe_sign_display = Nothing<SignDisplay>();
  maybe_sign_display = GetStringOption<SignDisplay>(
      isolate, options, "signDisplay", service,
      {"auto", "never", "always", "exceptZero", "negative"},
      {SignDisplay::AUTO, SignDisplay::NEVER, SignDisplay::ALWAYS,
       SignDisplay::EXCEPT_ZERO, SignDisplay::NEGATIVE},
      SignDisplay::AUTO);
  MAYBE_RETURN(maybe_sign_display, MaybeHandle<JSNumberFormat>());
  SignDisplay sign_display = maybe_sign_display.FromJust();

  // 33. Set numberFormat.[[SignDisplay]] to signDisplay.
  // The default sign in ICU is UNUM_SIGN_AUTO which is mapped from
  // SignDisplay::AUTO and CurrencySign::STANDARD so we can skip setting
  // under that values for optimization.
  if (sign_display != SignDisplay::AUTO ||
      currency_sign != CurrencySign::STANDARD) {
    settings = settings.sign(ToUNumberSignDisplay(sign_display, currency_sign));
  }

  // X. Let roundingMode be ? GetOption(options, "roundingMode", "string",
  // « "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor",
  // "halfExpand", "halfTrunc", "halfEven" »,
  // "halfExpand").
  Maybe<IntlRoundingMode> maybe_rounding_mode =
      GetStringOption<IntlRoundingMode>(
          isolate, options, "roundingMode", service,
          {"ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor",
           "halfExpand", "halfTrunc", "halfEven"},
          {IntlRoundingMode::CEIL, IntlRoundingMode::FLOOR,
           IntlRoundingMode::EXPAND, IntlRoundingMode::TRUNC,
           IntlRoundingMode::HALF_CEIL, IntlRoundingMode::HALF_FLOOR,
           IntlRoundingMode::HALF_EXPAND, IntlRoundingMode::HALF_TRUNC,
           IntlRoundingMode::HALF_EVEN},
          IntlRoundingMode::HALF_EXPAND);
  MAYBE_RETURN(maybe_rounding_mode, MaybeHandle<JSNumberFormat>());
  IntlRoundingMode rounding_mode = maybe_rounding_mode.FromJust();
  settings = settings.roundingMode(ToUNumberFormatRoundingMode(rounding_mode));

  // 25. Let dataLocaleData be localeData.[[<dataLocale>]].
  //
  // 26. Let patterns be dataLocaleData.[[patterns]].
  //
  // 27. Assert: patterns is a record (see 11.3.3).
  //
  // 28. Let stylePatterns be patterns.[[<style>]].
  //
  // 29. Set numberFormat.[[PositivePattern]] to
  // stylePatterns.[[positivePattern]].
  //
  // 30. Set numberFormat.[[NegativePattern]] to
  // stylePatterns.[[negativePattern]].
  //
  icu::number::LocalizedNumberFormatter fmt = settings.locale(icu_locale);

  Handle<Managed<icu::number::LocalizedNumberFormatter>>
      managed_number_formatter =
          Managed<icu::number::LocalizedNumberFormatter>::FromRawPtr(
              isolate, 0, new icu::number::LocalizedNumberFormatter(fmt));

  // Now all properties are ready, so we can allocate the result object.
  Handle<JSNumberFormat> number_format = Handle<JSNumberFormat>::cast(
      isolate->factory()->NewFastOrSlowJSObjectFromMap(map));
  DisallowGarbageCollection no_gc;
  number_format->set_locale(*locale_str);

  number_format->set_icu_number_formatter(*managed_number_formatter);
  number_format->set_bound_format(*factory->undefined_value());

  // 31. Return numberFormat.
  return number_format;
}

namespace {

icu::number::FormattedNumber FormatDecimalString(
    Isolate* isolate,
    const icu::number::LocalizedNumberFormatter& number_format,
    Handle<String> string, UErrorCode& status) {
  string = String::Flatten(isolate, string);
  DisallowGarbageCollection no_gc;
  const String::FlatContent& flat = string->GetFlatContent(no_gc);
  int32_t length = string->length();
  if (flat.IsOneByte()) {
    const char* char_buffer =
        reinterpret_cast<const char*>(flat.ToOneByteVector().begin());
    return number_format.formatDecimal({char_buffer, length}, status);
  }
  return number_format.formatDecimal({string->ToCString().get(), length},
                                     status);
}

}  // namespace

bool IntlMathematicalValue::IsNaN() const { return value_->IsNaN(); }

MaybeHandle<String> IntlMathematicalValue::ToString(Isolate* isolate) const {
  Handle<String> string;
  if (value_->IsNumber()) {
    return isolate->factory()->NumberToString(value_);
  }
  if (value_->IsBigInt()) {
    return BigInt::ToString(isolate, Handle<BigInt>::cast(value_));
  }
  DCHECK(value_->IsString());
  return Handle<String>::cast(value_);
}

namespace {
Maybe<icu::number::FormattedNumber> IcuFormatNumber(
    Isolate* isolate,
    const icu::number::LocalizedNumberFormatter& number_format,
    Handle<Object> numeric_obj) {
  icu::number::FormattedNumber formatted;
  // If it is BigInt, handle it differently.
  UErrorCode status = U_ZERO_ERROR;
  if (numeric_obj->IsBigInt()) {
    Handle<BigInt> big_int = Handle<BigInt>::cast(numeric_obj);
    Handle<String> big_int_string;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, big_int_string,
                                     BigInt::ToString(isolate, big_int),
                                     Nothing<icu::number::FormattedNumber>());
    big_int_string = String::Flatten(isolate, big_int_string);
    DisallowGarbageCollection no_gc;
    const String::FlatContent& flat = big_int_string->GetFlatContent(no_gc);
    int32_t length = big_int_string->length();
    DCHECK(flat.IsOneByte());
    const char* char_buffer =
        reinterpret_cast<const char*>(flat.ToOneByteVector().begin());
    formatted = number_format.formatDecimal({char_buffer, length}, status);
  } else {
    if (numeric_obj->IsString()) {
      // TODO(ftang) Correct the handling of string after the resolution of
      // https://github.com/tc39/proposal-intl-numberformat-v3/pull/82
      Handle<String> string =
          String::Flatten(isolate, Handle<String>::cast(numeric_obj));
      DisallowGarbageCollection no_gc;
      const String::FlatContent& flat = string->GetFlatContent(no_gc);
      int32_t length = string->length();
      if (flat.IsOneByte()) {
        const char* char_buffer =
            reinterpret_cast<const char*>(flat.ToOneByteVector().begin());
        formatted = number_format.formatDecimal({char_buffer, length}, status);
      } else {
        // We may have two bytes string such as "漢 123456789".substring(2)
        // The value will be "123456789" only in ASCII range, but encoded
        // in two bytes string.
        // ICU accepts UTF8 string, so if the source is two-byte encoded,
        // copy into a UTF8 string via ToCString.
        formatted = number_format.formatDecimal(
            {string->ToCString().get(), string->length()}, status);
      }
    } else {
      double number = numeric_obj->IsNaN()
                          ? std::numeric_limits<double>::quiet_NaN()
                          : numeric_obj->Number();
      formatted = number_format.formatDouble(number, status);
    }
  }
  if (U_FAILURE(status)) {
    // This happen because of icu data trimming trim out "unit".
    // See https://bugs.chromium.org/p/v8/issues/detail?id=8641
    THROW_NEW_ERROR_RETURN_VALUE(isolate,
                                 NewTypeError(MessageTemplate::kIcuError),
                                 Nothing<icu::number::FormattedNumber>());
  }
  return Just(std::move(formatted));
}

}  // namespace

Maybe<icu::number::FormattedNumber> IntlMathematicalValue::FormatNumeric(
    Isolate* isolate,
    const icu::number::LocalizedNumberFormatter& number_format,
    const IntlMathematicalValue& x) {
  if (x.value_->IsString()) {
    Handle<String> string;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, string, x.ToString(isolate),
                                     Nothing<icu::number::FormattedNumber>());
    UErrorCode status = U_ZERO_ERROR;
    icu::number::FormattedNumber result =
        FormatDecimalString(isolate, number_format, string, status);
    if (U_FAILURE(status)) {
      THROW_NEW_ERROR_RETURN_VALUE(isolate,
                                   NewTypeError(MessageTemplate::kIcuError),
                                   Nothing<icu::number::FormattedNumber>());
    }
    return Just(std::move(result));
  }
  CHECK(x.value_->IsNumber() || x.value_->IsBigInt());
  return IcuFormatNumber(isolate, number_format, x.value_);
}

Maybe<icu::number::FormattedNumberRange> IntlMathematicalValue::FormatRange(
    Isolate* isolate,
    const icu::number::LocalizedNumberRangeFormatter& number_range_format,
    const IntlMathematicalValue& x, const IntlMathematicalValue& y) {
  icu::Formattable x_formatable;
  MAYBE_ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, x_formatable, x.ToFormattable(isolate),
      Nothing<icu::number::FormattedNumberRange>());

  icu::Formattable y_formatable;
  MAYBE_ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, y_formatable, y.ToFormattable(isolate),
      Nothing<icu::number::FormattedNumberRange>());

  UErrorCode status = U_ZERO_ERROR;
  icu::number::FormattedNumberRange result =
      number_range_format.formatFormattableRange(x_formatable, y_formatable,
                                                 status);
  if (U_FAILURE(status)) {
    THROW_NEW_ERROR_RETURN_VALUE(isolate,
                                 NewTypeError(MessageTemplate::kIcuError),
                                 Nothing<icu::number::FormattedNumberRange>());
  }

  return Just(std::move(result));
}

namespace {
// Return the index of the end of leading white space or line terminator
// and the index of the start of trailing white space or line terminator.
template <typename Char>
std::pair<int, int> FindLeadingAndTrailingWhiteSpaceOrLineTerminator(
    base::Vector<const Char> src) {
  size_t leading_end = 0;

  // Find the length of leading StrWhiteSpaceChar.
  while (leading_end < src.size() &&
         IsWhiteSpaceOrLineTerminator(
             static_cast<uint16_t>(src.at(leading_end)))) {
    leading_end++;
  }
  size_t trailing_start = src.size();
  // Find the start of the trailing StrWhiteSpaceChar
  while (trailing_start > leading_end &&
         IsWhiteSpaceOrLineTerminator(
             static_cast<uint16_t>(src.at(trailing_start - 1)))) {
    trailing_start--;
  }
  return std::make_pair(leading_end, trailing_start);
}

Handle<String> TrimWhiteSpaceOrLineTerminator(Isolate* isolate,
                                              Handle<String> string) {
  string = String::Flatten(isolate, string);
  std::pair<int, int> whitespace_offsets;
  {
    DisallowGarbageCollection no_gc;
    String::FlatContent flat = string->GetFlatContent(no_gc);
    if (flat.IsOneByte()) {
      whitespace_offsets = FindLeadingAndTrailingWhiteSpaceOrLineTerminator(
          flat.ToOneByteVector());
    } else {
      whitespace_offsets =
          FindLeadingAndTrailingWhiteSpaceOrLineTerminator(flat.ToUC16Vector());
    }
  }
  if (whitespace_offsets.first == 0 &&
      string->length() == whitespace_offsets.second) {
    return string;
  }
  return isolate->factory()->NewSubString(string, whitespace_offsets.first,
                                          whitespace_offsets.second);
}

}  // namespace

// #sec-tointlmathematicalvalue
Maybe<IntlMathematicalValue> IntlMathematicalValue::From(Isolate* isolate,
                                                         Handle<Object> value) {
  Factory* factory = isolate->factory();
  // 1. Let primValue be ? ToPrimitive(value, number).
  Handle<Object> prim_value;
  if (value->IsJSReceiver()) {
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, prim_value,
        JSReceiver::ToPrimitive(isolate, Handle<JSReceiver>::cast(value),
                                ToPrimitiveHint::kNumber),
        Nothing<IntlMathematicalValue>());
  } else {
    prim_value = value;
  }
  IntlMathematicalValue result;
  // 2. If Type(primValue) is BigInt, return the mathematical value of
  // primValue.
  if (prim_value->IsBigInt()) {
    result.value_ = prim_value;
    result.approx_ = Handle<BigInt>::cast(prim_value)->AsInt64();
    return Just(result);
  }
  if (prim_value->IsOddball()) {
    prim_value = Oddball::ToNumber(isolate, Handle<Oddball>::cast(prim_value));
  }
  if (prim_value->IsNumber()) {
    result.value_ = prim_value;
    result.approx_ = prim_value->Number();
    return Just(result);
  }
  if (!prim_value->IsString()) {
    // No need to convert from Number to String, just call ToNumber.
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, result.value_,
                                     Object::ToNumber(isolate, prim_value),
                                     Nothing<IntlMathematicalValue>());
    result.approx_ = result.value_->Number();
    return Just(result);
  }
  Handle<String> string = Handle<String>::cast(prim_value);

  string = TrimWhiteSpaceOrLineTerminator(isolate, string);
  if (string->length() == 0) {
    result.value_ = handle(Smi::zero(), isolate);
    result.approx_ = 0;
    return Just(result);
  }
  // We may have a NonDecimalIntegerLiteral:
  if (2 < string->length() && string->Get(0) == '0') {
    uint16_t ch = string->Get(1);
    if (ch == 'b' || ch == 'B' || ch == 'o' || ch == 'O' || ch == 'x' ||
        ch == 'X') {
      result.approx_ = StringToDouble(
          isolate, string, ALLOW_HEX | ALLOW_OCTAL | ALLOW_BINARY, 0);
      // If approx is within the precision, just return as Number.
      if (result.approx_ < kMaxSafeInteger) {
        result.value_ = isolate->factory()->NewNumber(result.approx_);
        return Just(result);
      }
      // Otherwise return the BigInt
      MaybeHandle<BigInt> maybe_bigint = StringToBigInt(isolate, string);
      // If the parsing of BigInt fail, return nan
      if (maybe_bigint.is_null()) {
        isolate->clear_pending_exception();
        result.value_ = factory->nan_value();
        return Just(result);
      }
      result.value_ = maybe_bigint.ToHandleChecked();
      return Just(result);
    }
  }
  // If it does not fit StrDecimalLiteral StrWhiteSpace_opt, StringToDouble will
  // parse it as NaN, in that case, return NaN.
  result.approx_ = StringToDouble(isolate, string, NO_CONVERSION_FLAGS, 0);
  if (std::isnan(result.approx_)) {
    result.value_ = factory->nan_value();
    return Just(result);
  }
  // Handle Infinity / +Infinity / -Infinity
  if (!std::isfinite(result.approx_)) {
    if (result.approx_ < 0) {
      result.value_ = factory->minus_infinity_value();
    } else {
      result.value_ = factory->infinity_value();
    }
    return Just(result);
  }
  // At this point, str is for sure fit
  // "StrNumericLiteral StrWhiteSpace_opt" excluding "(+|-)?Infinity"
  result.value_ = string;

  return Just(result);
}

Maybe<icu::Formattable> IntlMathematicalValue::ToFormattable(
    Isolate* isolate) const {
  if (value_->IsNumber()) {
    return Just(icu::Formattable(approx_));
  }
  Handle<String> string;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, string, ToString(isolate),
                                   Nothing<icu::Formattable>());
  UErrorCode status = U_ZERO_ERROR;
  {
    DisallowGarbageCollection no_gc;
    const String::FlatContent& flat = string->GetFlatContent(no_gc);
    int length = string->length();
    if (flat.IsOneByte()) {
      icu::Formattable result(
          {reinterpret_cast<const char*>(flat.ToOneByteVector().begin()),
           length},
          status);
      if (U_SUCCESS(status)) return Just(result);
    } else {
      icu::Formattable result({string->ToCString().get(), length}, status);
      if (U_SUCCESS(status)) return Just(result);
    }
  }
  THROW_NEW_ERROR_RETURN_VALUE(isolate,
                               NewTypeError(MessageTemplate::kIcuError),
                               Nothing<icu::Formattable>());
}

namespace {
bool cmp_NumberFormatSpan(const NumberFormatSpan& a,
                          const NumberFormatSpan& b) {
  // Regions that start earlier should be encountered earlier.
  if (a.begin_pos < b.begin_pos) return true;
  if (a.begin_pos > b.begin_pos) return false;
  // For regions that start in the same place, regions that last longer should
  // be encountered earlier.
  if (a.end_pos < b.end_pos) return false;
  if (a.end_pos > b.end_pos) return true;
  // For regions that are exactly the same, one of them must be the "literal"
  // backdrop we added, which has a field_id of -1, so consider higher field_ids
  // to be later.
  return a.field_id < b.field_id;
}

}  // namespace

// Flattens a list of possibly-overlapping "regions" to a list of
// non-overlapping "parts". At least one of the input regions must span the
// entire space of possible indexes. The regions parameter will sorted in-place
// according to some criteria; this is done for performance to avoid copying the
// input.
std::vector<NumberFormatSpan> FlattenRegionsToParts(
    std::vector<NumberFormatSpan>* regions) {
  // The intention of this algorithm is that it's used to translate ICU "fields"
  // to JavaScript "parts" of a formatted string. Each ICU field and JavaScript
  // part has an integer field_id, which corresponds to something like "grouping
  // separator", "fraction", or "percent sign", and has a begin and end
  // position. Here's a diagram of:

  // var nf = new Intl.NumberFormat(['de'], {style:'currency',currency:'EUR'});
  // nf.formatToParts(123456.78);

  //               :       6
  //  input regions:    0000000211 7
  // ('-' means -1):    ------------
  // formatted string: "123.456,78 €"
  // output parts:      0006000211-7

  // To illustrate the requirements of this algorithm, here's a contrived and
  // convoluted example of inputs and expected outputs:

  //              :          4
  //              :      22 33    3
  //              :      11111   22
  // input regions:     0000000  111
  //              :     ------------
  // formatted string: "abcdefghijkl"
  // output parts:      0221340--231
  // (The characters in the formatted string are irrelevant to this function.)

  // We arrange the overlapping input regions like a mountain range where
  // smaller regions are "on top" of larger regions, and we output a birds-eye
  // view of the mountains, so that smaller regions take priority over larger
  // regions.
  std::sort(regions->begin(), regions->end(), cmp_NumberFormatSpan);
  std::vector<size_t> overlapping_region_index_stack;
  // At least one item in regions must be a region spanning the entire string.
  // Due to the sorting above, the first item in the vector will be one of them.
  overlapping_region_index_stack.push_back(0);
  NumberFormatSpan top_region = regions->at(0);
  size_t region_iterator = 1;
  int32_t entire_size = top_region.end_pos;

  std::vector<NumberFormatSpan> out_parts;

  // The "climber" is a cursor that advances from left to right climbing "up"
  // and "down" the mountains. Whenever the climber moves to the right, that
  // represents an item of output.
  int32_t climber = 0;
  while (climber < entire_size) {
    int32_t next_region_begin_pos;
    if (region_iterator < regions->size()) {
      next_region_begin_pos = regions->at(region_iterator).begin_pos;
    } else {
      // finish off the rest of the input by proceeding to the end.
      next_region_begin_pos = entire_size;
    }

    if (climber < next_region_begin_pos) {
      while (top_region.end_pos < next_region_begin_pos) {
        if (climber < top_region.end_pos) {
          // step down
          out_parts.push_back(NumberFormatSpan(top_region.field_id, climber,
                                               top_region.end_pos));
          climber = top_region.end_pos;
        } else {
          // drop down
        }
        overlapping_region_index_stack.pop_back();
        top_region = regions->at(overlapping_region_index_stack.back());
      }
      if (climber < next_region_begin_pos) {
        // cross a plateau/mesa/valley
        out_parts.push_back(NumberFormatSpan(top_region.field_id, climber,
                                             next_region_begin_pos));
        climber = next_region_begin_pos;
      }
    }
    if (region_iterator < regions->size()) {
      overlapping_region_index_stack.push_back(region_iterator++);
      top_region = regions->at(overlapping_region_index_stack.back());
    }
  }
  return out_parts;
}

namespace {
Maybe<int> ConstructParts(Isolate* isolate,
                          const icu::FormattedValue& formatted,
                          Handle<JSArray> result, int start_index,
                          bool style_is_unit, bool is_nan, bool output_source) {
  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString formatted_text = formatted.toString(status);
  if (U_FAILURE(status)) {
    THROW_NEW_ERROR_RETURN_VALUE(
        isolate, NewTypeError(MessageTemplate::kIcuError), Nothing<int>());
  }
  int32_t length = formatted_text.length();
  int index = start_index;
  if (length == 0) return Just(index);

  std::vector<NumberFormatSpan> regions;
  // Add a "literal" backdrop for the entire string. This will be used if no
  // other region covers some part of the formatted string. It's possible
  // there's another field with exactly the same begin and end as this backdrop,
  // in which case the backdrop's field_id of -1 will give it lower priority.
  regions.push_back(NumberFormatSpan(-1, 0, formatted_text.length()));
  Intl::FormatRangeSourceTracker tracker;
  {
    icu::ConstrainedFieldPosition cfpos;
    while (formatted.nextPosition(cfpos, status)) {
      int32_t category = cfpos.getCategory();
      int32_t field = cfpos.getField();
      int32_t start = cfpos.getStart();
      int32_t limit = cfpos.getLimit();
      if (category == UFIELD_CATEGORY_NUMBER_RANGE_SPAN) {
        DCHECK_LE(field, 2);
        tracker.Add(field, start, limit);
      } else {
        regions.push_back(NumberFormatSpan(field, start, limit));
      }
    }
  }

  std::vector<NumberFormatSpan> parts = FlattenRegionsToParts(&regions);

  for (auto it = parts.begin(); it < parts.end(); it++) {
    NumberFormatSpan part = *it;
    Handle<String> field_type_string = isolate->factory()->literal_string();
    if (part.field_id != -1) {
      if (style_is_unit && static_cast<UNumberFormatFields>(part.field_id) ==
                               UNUM_PERCENT_FIELD) {
        // Special case when style is unit.
        field_type_string = isolate->factory()->unit_string();
      } else {
        field_type_string =
            Intl::NumberFieldToType(isolate, part, formatted_text, is_nan);
      }
    }
    Handle<String> substring;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, substring,
        Intl::ToString(isolate, formatted_text, part.begin_pos, part.end_pos),
        Nothing<int>());

    if (output_source) {
      Intl::AddElement(
          isolate, result, index, field_type_string, substring,
          isolate->factory()->source_string(),
          Intl::SourceString(isolate,
                             tracker.GetSource(part.begin_pos, part.end_pos)));
    } else {
      Intl::AddElement(isolate, result, index, field_type_string, substring);
    }
    ++index;
  }
  JSObject::ValidateElements(*result);
  return Just(index);
}

// #sec-partitionnumberrangepattern
template <typename T, MaybeHandle<T> (*F)(
                          Isolate*, const icu::FormattedValue&,
                          const icu::number::LocalizedNumberFormatter&, bool)>
MaybeHandle<T> PartitionNumberRangePattern(Isolate* isolate,
                                           Handle<JSNumberFormat> number_format,
                                           Handle<Object> start,
                                           Handle<Object> end,
                                           const char* func_name) {
  Factory* factory = isolate->factory();
  // 4. Let x be ? ToIntlMathematicalValue(start).
  IntlMathematicalValue x;
  MAYBE_ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, x, IntlMathematicalValue::From(isolate, start), Handle<T>());

  // 5. Let y be ? ToIntlMathematicalValue(end).
  IntlMathematicalValue y;
  MAYBE_ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, y, IntlMathematicalValue::From(isolate, end), Handle<T>());

  // 1. If x is not-a-number or y is not-a-number, throw a RangeError exception.
  if (x.IsNaN()) {
    THROW_NEW_ERROR_RETURN_VALUE(
        isolate,
        NewRangeError(MessageTemplate::kInvalid,
                      factory->NewStringFromStaticChars("start"), start),
        MaybeHandle<T>());
  }
  if (y.IsNaN()) {
    THROW_NEW_ERROR_RETURN_VALUE(
        isolate,
        NewRangeError(MessageTemplate::kInvalid,
                      factory->NewStringFromStaticChars("end"), end),
        MaybeHandle<T>());
  }

  Maybe<icu::number::LocalizedNumberRangeFormatter> maybe_range_formatter =
      JSNumberFormat::GetRangeFormatter(
          isolate, number_format->locale(),
          *number_format->icu_number_formatter().raw());
  MAYBE_RETURN(maybe_range_formatter, MaybeHandle<T>());

  icu::number::LocalizedNumberRangeFormatter nrfmt =
      maybe_range_formatter.FromJust();

  Maybe<icu::number::FormattedNumberRange> maybe_formatted =
      IntlMathematicalValue::FormatRange(isolate, nrfmt, x, y);
  MAYBE_RETURN(maybe_formatted, Handle<T>());
  icu::number::FormattedNumberRange formatted =
      std::move(maybe_formatted).FromJust();

  return F(isolate, formatted, *(number_format->icu_number_formatter().raw()),
           false /* is_nan */);
}

MaybeHandle<String> FormatToString(Isolate* isolate,
                                   const icu::FormattedValue& formatted,
                                   const icu::number::LocalizedNumberFormatter&,
                                   bool) {
  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString result = formatted.toString(status);
  if (U_FAILURE(status)) {
    THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kIcuError), String);
  }
  return Intl::ToString(isolate, result);
}

MaybeHandle<JSArray> FormatToJSArray(
    Isolate* isolate, const icu::FormattedValue& formatted,
    const icu::number::LocalizedNumberFormatter& nfmt, bool is_nan,
    bool output_source) {
  UErrorCode status = U_ZERO_ERROR;
  bool is_unit = Style::UNIT == StyleFromSkeleton(nfmt.toSkeleton(status));
  CHECK(U_SUCCESS(status));

  Factory* factory = isolate->factory();
  Handle<JSArray> result = factory->NewJSArray(0);

  int format_to_parts;
  MAYBE_ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, format_to_parts,
      ConstructParts(isolate, formatted, result, 0, is_unit, is_nan,
                     output_source),
      Handle<JSArray>());
  USE(format_to_parts);

  return result;
}

MaybeHandle<JSArray> FormatRangeToJSArray(
    Isolate* isolate, const icu::FormattedValue& formatted,
    const icu::number::LocalizedNumberFormatter& nfmt, bool is_nan) {
  return FormatToJSArray(isolate, formatted, nfmt, is_nan, true);
}

}  // namespace

Maybe<icu::number::LocalizedNumberRangeFormatter>
JSNumberFormat::GetRangeFormatter(
    Isolate* isolate, String locale,
    const icu::number::LocalizedNumberFormatter& number_formatter) {
  UErrorCode status = U_ZERO_ERROR;
  UParseError perror;
  icu::number::LocalizedNumberRangeFormatter range_formatter =
      icu::number::UnlocalizedNumberRangeFormatter()
          .numberFormatterBoth(icu::number::NumberFormatter::forSkeleton(
              number_formatter.toSkeleton(status), perror, status))
          .locale(
              icu::Locale::forLanguageTag(locale.ToCString().get(), status));
  if (U_FAILURE(status)) {
    THROW_NEW_ERROR_RETURN_VALUE(
        isolate, NewTypeError(MessageTemplate::kIcuError),
        Nothing<icu::number::LocalizedNumberRangeFormatter>());
  }
  return Just(range_formatter);
}

MaybeHandle<String> JSNumberFormat::FormatNumeric(
    Isolate* isolate,
    const icu::number::LocalizedNumberFormatter& number_format,
    Handle<Object> numeric_obj) {
  Maybe<icu::number::FormattedNumber> maybe_format =
      IcuFormatNumber(isolate, number_format, numeric_obj);
  MAYBE_RETURN(maybe_format, Handle<String>());
  icu::number::FormattedNumber formatted = std::move(maybe_format).FromJust();

  return FormatToString(isolate, formatted, number_format,
                        numeric_obj->IsNaN());
}

MaybeHandle<String> JSNumberFormat::NumberFormatFunction(
    Isolate* isolate, Handle<JSNumberFormat> number_format,
    Handle<Object> value) {
  icu::number::LocalizedNumberFormatter* fmt =
      number_format->icu_number_formatter().raw();
  CHECK_NOT_NULL(fmt);

  // 4. Let x be ? ToIntlMathematicalValue(value).
  IntlMathematicalValue x;
  MAYBE_ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, x, IntlMathematicalValue::From(isolate, value),
      Handle<String>());

  // 5. Return FormatNumeric(nf, x).
  Maybe<icu::number::FormattedNumber> maybe_formatted =
      IntlMathematicalValue::FormatNumeric(isolate, *fmt, x);
  MAYBE_RETURN(maybe_formatted, Handle<String>());
  icu::number::FormattedNumber formatted =
      std::move(maybe_formatted).FromJust();

  return FormatToString(isolate, formatted, *fmt, x.IsNaN());
}

MaybeHandle<JSArray> JSNumberFormat::FormatToParts(
    Isolate* isolate, Handle<JSNumberFormat> number_format,
    Handle<Object> numeric_obj) {
  icu::number::LocalizedNumberFormatter* fmt =
      number_format->icu_number_formatter().raw();
  DCHECK_NOT_NULL(fmt);
  IntlMathematicalValue value;
  MAYBE_ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, value, IntlMathematicalValue::From(isolate, numeric_obj),
      Handle<JSArray>());

  Maybe<icu::number::FormattedNumber> maybe_formatted =
      IntlMathematicalValue::FormatNumeric(isolate, *fmt, value);
  MAYBE_RETURN(maybe_formatted, Handle<JSArray>());
  icu::number::FormattedNumber formatted =
      std::move(maybe_formatted).FromJust();

  return FormatToJSArray(isolate, formatted, *fmt, value.IsNaN(), false);
}

// #sec-number-format-functions

MaybeHandle<String> JSNumberFormat::FormatNumericRange(
    Isolate* isolate, Handle<JSNumberFormat> number_format,
    Handle<Object> x_obj, Handle<Object> y_obj) {
  return PartitionNumberRangePattern<String, FormatToString>(
      isolate, number_format, x_obj, y_obj,
      "Intl.NumberFormat.prototype.formatRange");
}

MaybeHandle<JSArray> JSNumberFormat::FormatNumericRangeToParts(
    Isolate* isolate, Handle<JSNumberFormat> number_format,
    Handle<Object> x_obj, Handle<Object> y_obj) {
  return PartitionNumberRangePattern<JSArray, FormatRangeToJSArray>(
      isolate, number_format, x_obj, y_obj,
      "Intl.NumberFormat.prototype.formatRangeToParts");
}

namespace {

struct CheckNumberElements {
  static const char* key() { return "NumberElements"; }
  static const char* path() { return nullptr; }
};

}  // namespace

const std::set<std::string>& JSNumberFormat::GetAvailableLocales() {
  static base::LazyInstance<Intl::AvailableLocales<CheckNumberElements>>::type
      available_locales = LAZY_INSTANCE_INITIALIZER;
  return available_locales.Pointer()->Get();
}

}  // namespace internal
}  // namespace v8
