#include <sys/types.h>
#include <unistd.h>
#include "erlang_mod.h"
#include "erlang_cmd.h"
#include "erlang_listener.h"
#include "../../mod_fix.h"

int fixup_cmd_erlang_call_route(void** param, int param_no){
	if (param_no < 3)
		return fixup_spve_null(param, 1);
	if (param_no == 4) {
		return fixup_pvar_null(param, 1);
	}
	if (param_no == 3) {
		pv_elem_t *model=NULL;
		str s;
		s.s = (char*)(*param);
		s.len = strlen(s.s);
		if(s.len==0) {
			LM_ERR("cmd_erlang_call_route: param %d is empty string! please use erlang empty list [].\n", param_no);
			return -1;
		}
		if(pv_parse_format(&s ,&model) || model==NULL) {
			LM_ERR("cmd_erlang_call_route: wrong format [%s] for value param!\n", s.s);
			return -1;
		}
		*param = (void*)model;
		return 0;
	}
	LM_ERR("erlang_call_route takes exactly 4 parameters.\n");
	return -1;
}

int cmd_erlang_call_route(struct sip_msg* msg, char *cn , char *rp, char *ar, char *_ret_pv) {
#define AVP_PRINTBUF_SIZE 1024
	static char printbuf[AVP_PRINTBUF_SIZE];
	int printbuf_len, bytessent, status;
	ei_x_buff argbuf;
	struct nodes_list* node;
	struct erlang_cmd erl_cmd;
	erlang_pid erl_pid;
	str conname, regproc, ret;
	pv_spec_t *ret_pv;
	
	memset(&erl_cmd,0,sizeof(struct erlang_cmd));
	if(msg==NULL) {
	    LM_ERR("cmd_erlang_call_route: received null msg\n");
	    return -1;
	}
	if(fixup_get_svalue(msg, (gparam_p)cn, &conname)<0) {
	    LM_ERR("cmd_erlang_call_route: cannot get the connection name\n");
	    return -1;
	}
	for(node=nodes_lst;node;node=node->next) {
		LM_DBG("cmd_erlang_call_route: matching %s with %.*s\n",node->name,conname.len,conname.s);
		if(strcmp(node->name, conname.s)==0) break;
	}
	if(node==0){ 
		LM_ERR("cmd_erlang_call_route: no such connection %.*s\n",conname.len,conname.s);
		return -1;
	}

	if(fixup_get_svalue(msg, (gparam_p)rp, &regproc)<0) {
	    LM_ERR("cmd_erlang_call_route: cannot get the registered proc name\n");
	    return -1;
	}
	
//	if(fixup_get_svalue(msg, rt, &ret)<0) {
//	    LM_ERR("cannot get the ret pv_spec\n");
//	    return -1;
//	}
	printbuf_len = AVP_PRINTBUF_SIZE-1;
	if(pv_printf(msg, (pv_elem_p)ar, printbuf, &printbuf_len)<0 || printbuf_len<=0)
	{
		LM_ERR("erlang_cmd_call_route: cannot expand args expression.\n");
		return -1;
	}

	tm_cell_t *t = 0;
	t = tm_api.t_gett();
	if (t==NULL || t==T_UNDEFINED) {
	    if(tm_api.t_newtran(msg)<0) {
		LM_ERR("cmd_erlang_call_route: cannot create the transaction\n");
		return -1;
	    }
	    t = tm_api.t_gett();
	    if (t==NULL || t==T_UNDEFINED) {
		LM_ERR("cmd_erlang_call_route: cannot look up the transaction\n");
		return -1;
	    }
	}

	ret_pv = (pv_spec_t*)shm_malloc(sizeof(pv_spec_t));
	if (!ret_pv) {
		LM_ERR("no shm memory\n\n");
		return -1;
	}
	memcpy(ret_pv, (pv_spec_t *)_ret_pv, sizeof(pv_spec_t));

	LM_DBG("cmd_erlang_call_route:  %.*s %.*s %.*s\n",conname.len,conname.s,
			regproc.len,regproc.s, printbuf_len,printbuf);

	if (tm_api.t_suspend(msg, &(erl_cmd.tm_hash), &(erl_cmd.tm_label)) < 0) {
	    LM_ERR("cmd_erlang_call_route: t_suspend() failed\n");
	    shm_free(ret_pv);
	    return -1;
	}
	LM_DBG("cmd_erlang_call_route: request suspended hash=%d label=%d\n",erl_cmd.tm_hash,erl_cmd.tm_label);
	erl_cmd.cmd=ERLANG_CALL_ROUTE;
	erl_cmd.node=node;
	erl_cmd.reg_name=shm_strdup(&regproc);
	erl_cmd.ret_pv=ret_pv;
	erl_cmd.erlbuf=shm_malloc(AVP_PRINTBUF_SIZE);
	argbuf.buff=erl_cmd.erlbuf;
	argbuf.buffsz=AVP_PRINTBUF_SIZE-1;
	argbuf.index=0;
//	ei_x_encode_empty_list(&argbuf);
	ei_x_encode_version(&argbuf);
	// gen_call is 3-element tuple, first is atom $gen_call, 
	// second is {pid,ref} tuple and third is user data 
	ei_x_encode_tuple_header(&argbuf, 3);
	ei_x_encode_atom(&argbuf, "$gen_call");
	ei_x_encode_tuple_header(&argbuf, 2);
	// we are hacking erlang pid so we can have worker system pid here
	memcpy(&erl_pid,ei_self(&(node->ec)),sizeof(erlang_pid));
	erl_pid.num=erl_cmd.tm_hash;  /* put tm_suspend params as pid so listener can easily get those back in reply*/
	erl_pid.serial=erl_cmd.tm_label;
	ei_x_encode_pid(&argbuf, &erl_pid);
//	ei_x_encode_pid(&argbuf, ei_self(ec));
	erl_cmd.ref=shm_malloc(sizeof(erlang_ref));
	if(!erl_cmd.ref) {
	    LM_ERR("no shm");
	    return -1;
	}
	utils_mk_ref(&(node->ec),erl_cmd.ref);
	ei_x_encode_ref(&argbuf, erl_cmd.ref);
	//so much for pid, now encode rex call tuple {call, mod, fun, arg, user}
//	ei_x_encode_tuple_header(&argbuf, 5);
//	ei_x_encode_atom(&argbuf,"call");
//	ei_x_encode_atom_len(&argbuf, mod.s, mod.len);
//	ei_x_encode_atom_len(&argbuf, fun.s, fun.len);
	if(ei_x_format_wo_ver(&argbuf, printbuf)!=0) {
		LM_ERR("cannot fromat erlang binary from arg string\n");
		return -1;
	}
//	ei_x_encode_atom(&argbuf,"user");
	erl_cmd.erlbuf_len=argbuf.index;
	bytessent=write(pipe_fds[1], &erl_cmd, sizeof(erl_cmd));
	LM_DBG("cmd_erlang_call_route: exiting, sent %d  %d %s %d %p %p\n",bytessent,
			erl_cmd.cmd,erl_cmd.reg_name,erl_cmd.erlbuf_len,erl_cmd.erlbuf, erl_cmd.node);
	return 0;
}


int send_erlang_call_route(struct erlang_cmd *erl_cmd) {
    struct nodes_list *node;
    struct pending_cmd *cmd;

    node=erl_cmd->node;
    if(ei_reg_send(&(node->ec), node->fd, erl_cmd->reg_name,
		erl_cmd->erlbuf, erl_cmd->erlbuf_len)<0) {
	LM_ERR("send_erlang_call failed\n");
    return -1;
    }
    cmd=shm_malloc(sizeof(struct pending_cmd));
    if (cmd==0) {
	LM_ERR("no shm memory\n");
	return -2;
    }
    cmd->ret_pv=erl_cmd->ret_pv;
    cmd->cmd=erl_cmd->cmd;
    cmd->route_no=erl_cmd->route_no;
    cmd->num=erl_cmd->tm_hash & 0x7FFF;    //mask bits that got trough erlang pid
    cmd->serial=erl_cmd->tm_label & 0x1FFF;// --,,--
    cmd->tm_hash=erl_cmd->tm_hash;
    cmd->tm_label=erl_cmd->tm_label;
    cmd->refn0=(erl_cmd->ref->n)[0];
    cmd->refn1=(erl_cmd->ref->n)[1];
    cmd->refn2=(erl_cmd->ref->n)[2];
    cmd->next=pending_cmds;
    pending_cmds=cmd;
    return 0;
}
