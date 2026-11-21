#pragma once
#include "motion.h"
#include <algorithm>
namespace liquid::settings {
// Independent viewports retain velocity when wheel input reverses, and clamp after resize.
class ScrollMotion {
    docky::motion::Spring spring_;
    float maximum_=0;
public:
    ScrollMotion(){spring_.SetSpec(docky::motion::SpringSpec::FromResponse(0.18,1.0));spring_.SetRest(0.02,0.1);}
    float Position() const {return std::clamp(spring_.F(),0.f,maximum_);}
    float Target() const {return static_cast<float>(spring_.Target());}
    void Bounds(float maximum){maximum_=std::max(0.f,maximum);spring_.SetTarget(std::clamp(spring_.Target(),0.0,double(maximum_)));if(spring_.Value()>maximum_)spring_.SnapTo(maximum_);}
    void By(float distance){spring_.SetTarget(std::clamp(spring_.Target()+distance,0.0,double(maximum_)));}
    void Ensure(float top,float bottom,float viewportTop,float viewportBottom){
        const float y=Target();if(top-y<viewportTop)By(top-y-viewportTop);else if(bottom-y>viewportBottom)By(bottom-y-viewportBottom);
    }
    void Reset(){spring_.SnapTo(0);}
    bool Step(double dt,bool reduced){if(reduced){spring_.SnapTo(spring_.Target());return false;}return spring_.Step(dt);}
    bool Animating() const {return spring_.Animating();}
};
inline float SettingsBodyY(float screenY,float scroll){return screenY+scroll;}
}
