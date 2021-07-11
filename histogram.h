#ifndef _HISTOGRAM_H_
#define _HISTOGRAM_H_

#include <hdr/hdr_histogram.h>

struct Histogram {
    Histogram(unsigned long max_value) : max_value(max_value)
    {
        hdr_init(1UL, max_value, 3, &histogram);
    }

    Histogram(const Histogram& other)
    {
        max_value = other.max_value;
        hdr_init(1UL, max_value, 3, &histogram);
        hdr_add(histogram, other.histogram);
    }

    Histogram(Histogram&& other)
    {
        max_value = other.max_value;
        histogram = other.histogram;
        other.histogram = nullptr;
    }

    struct hdr_histogram* get() { return histogram; }
    const struct hdr_histogram* get() const { return histogram; }

    ~Histogram()
    {
        if (histogram) hdr_close(histogram);
    }

private:
    unsigned long max_value;
    struct hdr_histogram* histogram;
};

#endif
