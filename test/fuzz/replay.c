/*
 * Runs a fuzz target on the files it is given, without libFuzzer: how
 * check.sh keeps the inputs that once found a bug (test/fuzz/regress/).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
int LLVMFuzzerInitialize(int* argc, char*** argv);

int main(int argc, char** argv)
{
    LLVMFuzzerInitialize(&argc, &argv);
    for (int i = 1; i < argc; ++i)
    {
        FILE* f = fopen(argv[i], "rb");
        uint8_t* data;
        long size;
        if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
        {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 1;
        }
        data = (uint8_t*)malloc(size ? (size_t)size : 1u);
        if (!data || fread(data, 1, (size_t)size, f) != (size_t)size)
            return 1;
        fclose(f);
        LLVMFuzzerTestOneInput(data, (size_t)size);
        free(data);
    }
    return 0;
}
