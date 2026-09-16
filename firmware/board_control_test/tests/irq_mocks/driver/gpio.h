#pragma once
using gpio_num_t = int;
constexpr int GPIO_NUM_15 = 15, GPIO_NUM_16 = 16;
int gpio_get_level(gpio_num_t);
