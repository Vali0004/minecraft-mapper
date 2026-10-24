// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <stdbool.h>
#include <string>
#include <stdint.h>

typedef struct nbt_callbacks_s nbt_callbacks;

struct nbt_callbacks_s {
  void (*tag1)(const nbt_callbacks *, const std::string key, uint8_t value);
  void (*tag2)(const nbt_callbacks *, const std::string key, uint16_t value);
  void (*tag3)(const nbt_callbacks *, const std::string key, int32_t value);
  void (*tag7)(const nbt_callbacks *, const std::string key, int size, const uint8_t *data);
  void (*tag8)(const nbt_callbacks *, const std::string key, const std::string value);
  void *state;
};

void parseTag(std::string name_in, uint8_t tag, const uint8_t *nbt, int *pos, const nbt_callbacks *cb, bool verbose);
void parseNBT(const unsigned char *nbt, int size, const nbt_callbacks *cb, bool verbose);
