#pragma once
#include "menu_model.h"
namespace liquid::menu {
// Row zero in a nested menu is its enabled Back item. Disabled items and separators are skipped.
inline int NextSelectable(const std::vector<MenuItem>& items,bool back,int current,int direction){
 const int count=static_cast<int>(items.size())+(back?1:0);if(count<=0||!direction)return -1;
 const int step=direction>0?1:-1;int row=current>=0&&current<count?current:(step>0?-1:0);
 for(int i=0;i<count;++i){row=(row+step+count)%count;if(back&&row==0)return row;
  const auto& item=items[static_cast<size_t>(row-(back?1:0))];
  if(item.enabled&&item.kind!=ItemKind::Separator)return row;}
 return -1;
}
}
