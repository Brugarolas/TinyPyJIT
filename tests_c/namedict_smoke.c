#include "tinypy/objects/namedict.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    NameDict dict;
    NameDict__ctor(&dict, 0.66f);
    assert(dict.version != 0);
    uint64_t version = dict.version;

    NameKey k1 = {0x1234}, k2 = {0x5678};
    py_TValue v1 = {1, 10}, v2 = {1, 20};

    NameDict__set(&dict, &k1, &v1);
    assert(dict.version != version);
    version = dict.version;
    assert(NameDict__try_get(&dict, &k1)->value == 10);
    assert(NameDict__find_index(&dict, &k1) >= 0);

    v1.value = 11;
    NameDict__set(&dict, &k1, &v1);
    assert(dict.version != version);
    version = dict.version;
    assert(NameDict__try_get(&dict, &k1)->value == 11);

    NameDict__set(&dict, &k2, &v2);
    assert(dict.version != version);
    version = dict.version;
    assert(!NameDict__del(&dict, (py_Name)&(NameKey){0x9999}));
    assert(dict.version == version);
    assert(NameDict__del(&dict, &k1));
    assert(dict.version != version);
    assert(NameDict__try_get(&dict, &k1) == NULL);

    version = dict.version;
    NameDict__clear(&dict);
    assert(dict.version != version);
    assert(dict.length == 0);

    NameDict__dtor(&dict);
    puts("namedict smoke: ok");
    return 0;
}
