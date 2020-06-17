#include <array>
#include <cassert>
#include <regex>

#include "mega/filesystem.h"
#include "mega/filter.h"
#include "mega/utils.h"

namespace mega
{

class GlobFilter
  : public Filter
{
public:
    GlobFilter(const string& text, const bool inheritable, const FilterType type);

    bool match(const string& s) const override;

    FilterStrategy strategy() const override;
}; /* GlobFilter */

class RegexFilter
  : public Filter
{
public:
    RegexFilter(const string& text, const bool inheritable, const FilterType type);

    bool match(const string& s) const override;

    FilterStrategy strategy() const override;

private:
    // the compiled form of the filter text.
    std::regex mRegex;

    // common flags used by all regex filters.
    static const std::regex::flag_type mRegexFlags;
}; /* RegexFilter */

static bool isEmpty(const char* m, const char* n);

static bool syntaxError(const string& text);

const std::regex::flag_type RegexFilter::mRegexFlags =
  std::regex_constants::extended | std::regex_constants::optimize;

bool Filter::inheritable() const
{
    return mInheritable;
}

const string& Filter::text() const
{
    return mText;
}

FilterType Filter::type() const
{
    return mType;
}

Filter::~Filter()
{
}

Filter::Filter(const string& text, const bool inheritable, const FilterType type)
  : mText(text)
  , mInheritable(inheritable)
  , mType(type)
{
}

FilterClass::FilterClass()
  : mNames()
  , mPaths()
{
}

void FilterClass::add(FilterPtr&& filter)
{
    switch (filter->type())
    {
    case FT_NAME:
        mNames.emplace_back(std::move(filter));
        break;
    case FT_PATH:
        mPaths.emplace_back(std::move(filter));
        break;
    default:
        assert(!"Unknown filter type");
        break;
    }
}

void FilterClass::clear()
{
    mNames.clear();
    mPaths.clear();
}

bool FilterClass::empty() const
{
    return mNames.empty() && mPaths.empty();
}

bool FilterClass::match(const string_pair& p, const bool onlyInheritable) const
{
    for (auto &fp : mPaths)
    {
        if (onlyInheritable && !fp->inheritable())
        {
            LOG_debug << "Skipped uninheritable filter "
                      << toString(*fp);
            continue;
        }

        if (fp->match(p.second))
        {
            LOG_debug << p.second
                      << " matched by "
                      << toString(*fp);
            return true;
        }
    }

    for (auto &fn : mNames)
    {
        if (onlyInheritable && !fn->inheritable())
        {
            LOG_debug << "Skipped uninheritable filter "
                      << toString(*fn);
            continue;
        }

        if (fn->match(p.first))
        {
            LOG_debug << p.first
                      << " matched by "
                      << toString(*fn);
            return true;
        }
    }

    return false;
}

FilterChain::FilterChain()
  : mExclusions()
  , mInclusions()
{
}

bool FilterChain::add(const string& text)
{
    const char *m = text.data();
    const char *n = m + text.size();
    FilterType type;
    bool exclusion;
    bool inheritable = true;
    bool regex;

    // what class of filter is this?
    switch (*m++)
    {
    // exclusion filter.
    case '-':
        exclusion = true;
        break;
    // inclusion filter.
    case '+':
        exclusion = false;
        break;
    // invalid class of filter.
    default:
        return syntaxError(text);
    }

    // what type of filter is this?
    // default to name if not specified.
    switch (*m)
    {
    // name filter, not inherited.
    case 'N':
        inheritable = false;
    // name filter, inherited.
    case 'n':
        ++m;
    // default to inherited name filter.
    default:
        type = FT_NAME;
        break;
    // path filter, always inherited.
    case 'p':
        type = FT_PATH;
        ++m;
        break;
    }

    // what matching strategy does this filter use?
    // default to glob if not specified.
    switch (*m)
    {
    // glob strategy.
    case 'g':
        ++m;
    // default to glob strategy.
    default:
        regex = false;
        break;
    // regex strategy.
    case 'r':
        regex = true;
        ++m;
        break;
    }

    // make sure we're at the start of the pattern.
    if (*m++ != ':')
    {
        return syntaxError(text);
    }

    // is the pattern effectively empty?
    if (isEmpty(m, n))
    {
        return syntaxError(text);
    }

    // create the filter.
    FilterPtr filter;

    try
    {
        if (regex)
        {
            // this'll throw if the regex is malformed.
            filter.reset(new RegexFilter(m, inheritable, type));
        }
        else
        {
            filter.reset(new GlobFilter(m, inheritable, type));
        }
    }
    catch (std::regex_error&)
    {
        return syntaxError(text);
    }

    // add the filter.
    if (exclusion)
    {
        LOG_debug << "Adding exclusion " << toString(*filter);
        mExclusions.add(std::move(filter));
    }
    else
    {
        LOG_debug << "Adding inclusion " << toString(*filter);
        mInclusions.add(std::move(filter));
    }

    return true;
}

void FilterChain::clear()
{
    mExclusions.clear();
    mInclusions.clear();
}

bool FilterChain::empty() const
{
    return mExclusions.empty() && mInclusions.empty();
}

bool FilterChain::excluded(const string_pair& p, const bool onlyInheritable) const
{
    return mExclusions.match(p, onlyInheritable);
}

bool FilterChain::included(const string_pair& p, const bool onlyInheritable) const
{
    return mInclusions.match(p, onlyInheritable);
}

bool FilterChain::load(InputStreamAccess& isAccess)
{
    string_vector filters;

    // read the filters, line by line.
    // empty lines are omitted.
    if (!readLines(isAccess, filters))
    {
        return false;
    }

    // save the current filters in case of error.
    FilterClass exclusions(std::move(mExclusions));
    FilterClass inclusions(std::move(mInclusions));

    // make sure the filter vectors are in a well-defined state.
    clear();

    // add all the filters.
    for (const auto &f : filters)
    {
        // skip comments.
        if (f[0] == '#')
        {
            continue;
        }

        // try and add the filter.
        if (!add(f))
        {
            // restore previous filters.
            mExclusions = std::move(exclusions);
            mInclusions = std::move(inclusions);

            // changes are not committed.
            return false;
        }
    }

    // changes are committed.
    return true;
}

bool FilterChain::load(FileAccess& ifAccess)
{
    FileInputStream isAccess(&ifAccess);

    return load(isAccess);
}

const string& toString(const FilterStrategy strategy)
{
    static const std::array<string, NUM_FILTER_STRATEGIES> strings = {
        "GLOB",
        "REGEX"
    }; /* strings */

    assert(strategy < NUM_FILTER_STRATEGIES);

    return strings.at(strategy);
}

string toString(const Filter& filter)
{
    ostringstream osstream;

    osstream << toString(filter.type())
             << "/"
             << toString(filter.strategy())
             << ":"
             << filter.text();

    return osstream.str();
}

const string& toString(const FilterType type)
{
    static const std::array<string, NUM_FILTER_TYPES> strings = {
        "NAME",
        "PATH"
    }; /* strings */

    assert(type < NUM_FILTER_TYPES);

    return strings.at(type);
}

GlobFilter::GlobFilter(const string &text, const bool inheritable, const FilterType type)
  : Filter(text, inheritable, type)
{
}

bool GlobFilter::match(const string &s) const
{
    return wildcardMatch(s.c_str(), mText.c_str());
}

FilterStrategy GlobFilter::strategy() const
{
    return FS_GLOB;
}

RegexFilter::RegexFilter(const string& text, const bool inheritable, const FilterType type)
  : Filter(text, inheritable, type)
  , mRegex(text, mRegexFlags)
{
}

bool RegexFilter::match(const string& s) const
{
    return std::regex_match(s, mRegex);
}

FilterStrategy RegexFilter::strategy() const
{
    return FS_REGEX;
}

bool isEmpty(const char* m, const char* n)
{
    const char* w = m;

    while (m < n)
    {
        w += std::isspace(*m++) > 0;
    }

    return n == w;
}

bool syntaxError(const string& text)
{
    LOG_debug << "Syntax error parsing: " << text;

    return false;
}

} /* mega */

