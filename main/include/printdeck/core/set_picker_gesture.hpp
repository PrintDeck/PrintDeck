#pragma once
#include <cstdlib>

namespace printdeck::core {
// A drag owns its original touch target until release. Returning towards the
// start never turns it back into a tap, and a swipe commits only on release.
class SetPickerGesture {
 public:
  enum class Result { none, tap, previous, next };
  void press(int x, int y, bool known_drag=false) {
    x_=x;y_=y;peak_x_=peak_y_=0;active_=true;moved_=known_drag;result_=Result::none;
  }
  void move(int x, int y) {
    if(!active_)return;
    const int dx=x-x_,dy=y-y_;
    if(std::abs(dx)>std::abs(peak_x_))peak_x_=dx;
    if(std::abs(dy)>std::abs(peak_y_))peak_y_=dy;
    if(std::abs(dx)>4 || std::abs(dy)>4)moved_=true;
  }
  void mark_gesture() { if(active_)moved_=true; }
  void cancel() { active_=false;result_=Result::none; }
  Result release(int x, int y) {
    if(!active_)return Result::none;
    move(x,y);active_=false;
    if(std::abs(peak_x_)>=24 && std::abs(peak_x_)>std::abs(peak_y_))
      result_=peak_x_<0?Result::next:Result::previous;
    else result_=moved_?Result::none:Result::tap;
    return result_;
  }
  Result result() const { return result_; }
 private:
  int x_=0,y_=0,peak_x_=0,peak_y_=0;
  bool active_=false,moved_=false;
  Result result_=Result::none;
};
}
