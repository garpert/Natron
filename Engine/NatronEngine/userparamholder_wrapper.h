#ifndef SBK_USERPARAMHOLDERWRAPPER_H
#define SBK_USERPARAMHOLDERWRAPPER_H

#define protected public

#include <shiboken.h>

#include <NodeWrapper.h>

class UserParamHolderWrapper : public UserParamHolder
{
public:
    UserParamHolderWrapper();
    virtual ~UserParamHolderWrapper();
};

#endif // SBK_USERPARAMHOLDERWRAPPER_H

