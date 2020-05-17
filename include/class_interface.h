#ifndef CLASS_INTERFACE_H
#define CLASS_INTERFACE_H

#include "data.h"

#include "hmpdf.h"

typedef struct//{{{
{
    char *class_ini;
    char *class_pre;

    void /*struct precision*/ *pr;
    void /*struct background*/ *ba;
    void /*struct thermo*/ *th;
    void /*struct primordial*/ *pm;
    void /*struct perturbs*/ *pt;
    void /*struct nonlinear*/ *nl;
    void /*struct transfers*/ *tr;
    void /*struct spectra*/ *sp;
    void /*struct lensing*/ *le;
    void /*struct output*/ *op;
}//}}}
class_interface_t;

void null_class_interface(all_data *d);
void reset_class_interface(all_data *d);
void init_class_interface(all_data *d);

#endif
