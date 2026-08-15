#include "bootutil/sign_key.h"

extern const unsigned char ecdsa_pub_key[];
extern const unsigned int  ecdsa_pub_key_len;

const struct bootutil_key bootutil_keys[] =
{
    {
        .key = ecdsa_pub_key,
        .len = &ecdsa_pub_key_len,
    },
};

const int bootutil_key_cnt =
    (int) (sizeof(bootutil_keys) / sizeof(bootutil_keys[0]));
