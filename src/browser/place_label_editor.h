#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <string>

class Input;
class Label;
class Button;

namespace files {
  class PlaceLabelEditor final : public Flex {
  public:
    PlaceLabelEditor(
        float width, const std::string& label, std::function<void(std::string)> submit, std::function<void()> cancel
    );
    Input* input() const { return m_input; }
    void setSaving(bool saving);
    void showError(const std::string& error);

  private:
    void submit();
    Input* m_input = nullptr;
    Label* m_message = nullptr;
    Button* m_save = nullptr;
    Button* m_cancel = nullptr;
    bool m_saving = false;
    std::function<void(std::string)> m_submit;
  };
} // namespace files
