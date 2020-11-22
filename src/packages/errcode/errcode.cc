/*
 * errcode.h
 * Created by: zhang jun
 * Description: in order to set errcode to mud client.
 * Modification:
 *    2020-11-21 - zhang jun - initial creation
 */

#include "base/package_api.h"

#include "packages/errcode/errcode.h"


#ifdef F_SET_ERRCODE
void f_set_errcode(void){
    int ret = 0;
    if (sp->type == T_NUMBER) {
        set_errcode(sp->u.number);
        ret = 1;
    }

    put_number(ret);
    return;
}
#endif

int set_errcode(int ecode) {
    current_errcode = ecode;
    return 1;
}