/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmCursesWidget.h"

cmCursesWidget::cmCursesWidget(int width, int height, int left, int top)
{
  this->Field = new_field(height, width, top, left, 0, 0);
  set_field_userptr(this->Field, reinterpret_cast<char*>(this));
  field_opts_off(this->Field, O_AUTOSKIP);
  this->Page = 0;
}

cmCursesWidget::~cmCursesWidget()
{
  if (this->Field) {
    free_field(this->Field);
    this->Field = nullptr;
  }
}

void cmCursesWidget::Move(int x, int y, bool isNewPage)
{
  if (!this->Field) {
    return;
  }

  move_field(this->Field, y, x);
  if (isNewPage) {
    set_new_page(this->Field, true);
  } else {
    set_new_page(this->Field, false);
  }
}

void cmCursesWidget::SetValue(std::string const& value)
{
  this->Value = value;
  set_field_buffer(this->Field, 0, const_cast<char*>(value.c_str()));
}

char const* cmCursesWidget::GetValue()
{
  return this->Value.c_str();
}
