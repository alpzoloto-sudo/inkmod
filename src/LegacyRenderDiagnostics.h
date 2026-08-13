#pragma once
#include <Arduino.h>
namespace LegacyRenderDiagnostics {
enum class Key:uint8_t{Menu,Back};
inline uint8_t& step(){static uint8_t v=0;return v;}
inline uint32_t& lastMs(){static uint32_t v=0;return v;}
inline bool feed(Key k){constexpr uint32_t timeout=4500;uint32_t now=millis();if(lastMs()&&now-lastMs()>timeout)step()=0;lastMs()=now;static constexpr Key seq[5]={Key::Menu,Key::Back,Key::Menu,Key::Back,Key::Menu};if(k==seq[step()]){++step();if(step()==5){step()=0;lastMs()=0;return true;}return false;}step()=(k==Key::Menu)?1:0;return false;}
}
