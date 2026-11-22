/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "book_cover.h"
static bool stop(void *p){unsigned *calls=p;return ++*calls>=4;}
int main(int argc, char **argv) {
    assert(argc > 2);
    for (int i = 2; i < argc; ++i) {
        bool png = strstr(argv[i], ".png") != NULL;
        unsigned w = 0, h = 0;
        const unsigned ow = 176, oh = 240;
        uint8_t *pixels = malloc((size_t)ow * oh + 16);
        assert(pixels);
        memset(pixels, 0xcc, (size_t)ow * oh + 16);
        if (!strcmp(argv[1], "reject")) {
            assert(!book_image_file_grayscale(argv[i], png, ow, oh, pixels));
        } else {
            assert(book_image_file_dimensions(argv[i], png, &w, &h));
            assert(book_image_file_grayscale(argv[i], png, ow, oh, pixels));
            if (!strcmp(argv[1], "pixels")) {
                char reference[4096];
                assert(snprintf(reference,sizeof(reference),"%s.raw",argv[i])<(int)sizeof(reference));
                FILE *f=fopen(reference,"rb");assert(f);
                const size_t bytes=(size_t)ow*oh;
                uint8_t *expected=malloc(bytes*3),*gray=malloc(bytes),*alpha=malloc(bytes);
                assert(expected&&gray&&alpha&&fread(expected,bytes,3,f)==3&&fgetc(f)==EOF&&!fclose(f));
                assert(!memcmp(pixels,expected,bytes));
                assert(book_png_file_layer(argv[i],ow,oh,gray,alpha));
                assert(!memcmp(gray,expected+bytes,bytes)&&!memcmp(alpha,expected+2*bytes,bytes));
                free(expected);free(gray);free(alpha);
            }
            if (!strcmp(argv[1], "compare")) {
                char reference[4096];
                assert(snprintf(reference,sizeof(reference),"%s-rgb.png",argv[i])<(int)sizeof(reference));
                uint8_t *expected=malloc((size_t)ow*oh);assert(expected);
                assert(book_image_file_grayscale(reference,true,ow,oh,expected));
                assert(!memcmp(pixels,expected,(size_t)ow*oh));free(expected);
            }
            unsigned sum = 0;
            for (unsigned j = 0; j < ow * oh; ++j) sum += pixels[j];
            assert(sum > 30u * ow * oh && sum < 230u * ow * oh);
            unsigned calls=0;
            assert(!book_image_file_grayscale_cancel(argv[i],png,ow,oh,pixels,stop,&calls));
            assert(calls>=4);
            printf("decoded and cancelled: %s (%ux%u)\n", argv[i], w, h);
        }
        for (unsigned j = 0; j < 16; ++j) assert(pixels[(size_t)ow * oh + j] == 0xcc);
        free(pixels);
    }
}
