// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "flash_image.hpp"
#include "test_support.hpp"

int main(int argc, char **argv)
{
    test::Suite suite;
    const std::vector<std::string> validNumbers = {
        "0", "1", "00123", "4294967295", "0x0", "0X0", "0xabcdef", "0XABCDEF",
        "0x1234AbCd", "$filesize", std::string(256, '9')
    };
    for (size_t i = 0; i < validNumbers.size(); ++i) {
        suite.Add("valid-number-" + std::to_string(i), [value = validNumbers[i]] {
            REQUIRE(IsSafeUbootNumber(value));
        });
    }
    const std::vector<std::string> invalidNumbers = {
        "", "0x", "0X", "0xG", "0b10", "-1", "+1", "1.0", " 1", "1 ", "1\t2",
        "1\n", "1\r", "1;reset", "1&reset", "1|reset", "${filesize}", "$FILESIZE",
        "$filesize;reset", "0x12z", "\xEF\xBC\x91\xEF\xBC\x92", std::string("1\0", 2), std::string(1, '\xFF')
    };
    for (size_t i = 0; i < invalidNumbers.size(); ++i) {
        suite.Add("invalid-number-" + std::to_string(i), [value = invalidNumbers[i]] {
            REQUIRE(!IsSafeUbootNumber(value));
        });
    }
    const std::vector<std::string> validNames = {
        "a", "image.bin", "uNAND_full.img", "gen3_uboot.bin.usb", "Image.gz", "a-b_c+d.e",
        ".hidden", "...", "123", std::string(254, 'a'), std::string(255, 'a')
    };
    for (size_t i = 0; i < validNames.size(); ++i) {
        suite.Add("valid-filename-" + std::to_string(i), [value = validNames[i]] {
            REQUIRE(IsSafeUbootFilename(value));
        });
    }
    const std::vector<std::string> invalidNames = {
        "", ".", "..", std::string(256, 'a'), "a b", "a\tb", "a\nb", "a\rb", "a/b",
        "a\\b", "../a", "C:a", "a;reset", "a&b", "a|b", "a'b", "a\"b", "a$b",
        "a`b", "a(b)", "a{b}", "a*b", "a?b", "a<b", "a>b",
        std::string("a\0b", 3), std::string(1, '\x80'), std::string(1, '\xFF')
    };
    for (size_t i = 0; i < invalidNames.size(); ++i) {
        suite.Add("invalid-filename-" + std::to_string(i), [value = invalidNames[i]] {
            REQUIRE(!IsSafeUbootFilename(value));
        });
    }
    return suite.Run(argc, argv);
}
