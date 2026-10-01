/* Included inside upstream state.c, after its low-level read/write helpers.
 * Watcom int is 16 bits; gold, experience, RNG, cells and markers need 32. */
int rs_write_long(FILE *stream, long value)
{
    return rs_write(stream, &value, 4);
}

int rs_read_long(FILE *stream, long *value)
{
    return rs_read(stream, value, 4);
}

int rs_write_longs(FILE *stream, long *values, int count)
{
    int i;
    rs_write_int(stream, count);
    for (i = 0; i < count; ++i)
        rs_write_long(stream, values[i]);
    return WRITESTAT;
}

int rs_read_longs(FILE *stream, long *values, int count)
{
    int i, stored = 0;
    rs_read_int(stream, &stored);
    if (stored != count)
        format_error = TRUE;
    for (i = 0; i < count; ++i)
        rs_read_long(stream, &values[i]);
    return READSTAT;
}
