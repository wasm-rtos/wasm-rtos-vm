static unsigned data[6000];
static unsigned seed = 12345;
static unsigned next(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }
static void sort(unsigned* a, int lo, int hi)
{
    while (lo < hi)
    {
        unsigned p = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j)
        {
            while (a[i] < p) ++i;
            while (a[j] > p) --j;
            if (i <= j) { unsigned t = a[i]; a[i] = a[j]; a[j] = t; ++i; --j; }
        }
        if (j - lo < hi - i) { sort(a, lo, j); lo = i; } else { sort(a, i, hi); hi = j; }
    }
}
__attribute__((export_name("run"))) unsigned run(unsigned rounds)
{
    unsigned sum = 0;
    for (unsigned r = 0; r < rounds; ++r)
    {
        for (int i = 0; i < 6000; ++i) data[i] = next();
        sort(data, 0, 5999);
        for (int i = 0; i < 6000; i += 97) sum = sum * 31u + data[i];
    }
    return sum;
}
