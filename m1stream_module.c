/**
********************************************************************************
* @file     m1stream_module.c
* @author   Bachmann electronic GmbH
* @version  $Revision: 3.90 $ $LastChangedBy: BE $
* @date     $LastChangeDate: 2013-06-10 11:00:00 $
*
* @brief    This file contains all standard initialization-/deinitialization
*           routines which are required for a software module.
*           On starting the module, the system calls the function
*           m1stream_Init() which spawns the function m1stream_bTaskMain
*           as the communication task (b-Task).
*           If m1stream_Init returns "successfully", the SMI call
*           SMI_PROC_ENDOFINIT is sent in a second stage which brings
*           the module to the state RUN.
*
*           Normally it is not necessary to change this file,
*           all application specific work is done in the file m1stream_app.c.
*           Only the execution of module specific SMI calls (if used)
*           has to be added to m1stream_Main.
*
********************************************************************************
* COPYRIGHT BY BACHMANN ELECTRONIC GmbH 2013
*******************************************************************************/

/* VxWorks includes */
#include <vxWorks.h>
#include <string.h>
#include <taskLib.h>
#include <sigLib.h>
#include <stdio.h>
#include <setjmp.h>
#include <sysLib.h>
#include <symLib.h>
#include <sysSymTbl.h>

/* MSys includes */
#include <mtypes.h>
#include <msys_e.h>
#include <log_e.h>
#include <smi_e.h>
#include <res_e.h>
#include <mod_e.h>
#include <svi_e.h>
#include <prof_e.h>

/* Project includes */
#include "m1stream_e.h"
#include "m1stream_int.h"

/* Defines for SMI server task */
#define SMI_SRV_PRIO        120         /* Priority (range 118 ... 127) */
#define SMI_SRV_STACKSIZE   10000       /* Stack size in bytes */

/* Variable definitions */
SMI_ID *m1stream_pSmiId;              /* Id of module-SMI */
SINT32  m1stream_Debug = 0;           /* Debug level of module */
SINT32  m1stream_AppPrio = 0;         /* Task priority of module */
UINT32  m1stream_ModState;            /* Module state */
SEM_ID  m1stream_StateSema = 0;       /* Semaphore for halting tasks */
CHAR    m1stream_ProfileName[M_PATHLEN_A];    /* Path/Name of config file */
UINT32  m1stream_CfgLine = 0;         /* Start line in config file */
CHAR    m1stream_AppName[M_MODNAMELEN_A];     /* Instance name of module */
CHAR    m1stream_ModuleInfoDesc[SMI_DESCLEN_A];
UINT32  m1stream_SviHandle = 0;       /* SVI server handle */
MLOCAL jmp_buf JumpEnv;                 /* Jump Environment for 'longjmp' */
/* The file m1stream.ver will be generated by the C++ Developer tool */
CHAR    m1stream_Version[M_VERSTRGLEN_A] = {
#include "m1stream.ver"
};

/* Functions to be called from outside this file */
SINT32  m1stream_Init(MOD_CONF * pConf, MOD_LOAD * pLoad);

/* Functions to be called only from within this file */
MLOCAL SINT32 BaseInit(VOID);
MLOCAL VOID BaseDeinit(VOID);
MLOCAL VOID bTaskMain(VOID);
MLOCAL VOID RpcNull(SMI_MSG * pMsg);
MLOCAL VOID RpcReset(SMI_MSG * pMsg);
MLOCAL VOID RpcStop(SMI_MSG * pMsg);
MLOCAL VOID RpcRun(SMI_MSG * pMsg);
MLOCAL VOID RpcNewCfg(SMI_MSG * pMsg);
MLOCAL VOID RpcDeinit(SMI_MSG * pMsg);
MLOCAL VOID RpcSetDbg(SMI_MSG * pMsg);
MLOCAL VOID RpcGetInfo(SMI_MSG * pMsg);
MLOCAL VOID RpcEndOfInit(SMI_MSG * pMsg);
MLOCAL VOID PanicHandler(UINT32 PanicMode);

/* Function pointer for extended version of smi_receive and svi_MsgHandler */
MLOCAL FUNCPTR fpSmiReceive = NULL;
MLOCAL FUNCPTR fpSviMsgHandler = NULL;

/**
********************************************************************************
* @brief Entry point of the module.
*        Will be called after module loading as part of the module handler.
*        Initializes the interfaces to the environment
*        (SMI, SVI) so that they are ready to operate.
*
*        Further steps of initialization will be performed in RpcEndOfInit()
*        which will be called by SMI after the function m1stream_Init()
*        has returned.
*
* @param[in]  pConf   Parameter for configuration (data from MCONFIG.INI).
* @param[in]  pLoad   Parameter from module loading.
* @param[out] N/A
*
* @retval     >= 0 .. Task-Id, module initialized correctly
* @retval      < 0 .. Error during initialization
*******************************************************************************/
SINT32 m1stream_Init(MOD_CONF * pConf, MOD_LOAD * pLoad)
{
    SINT32  TskId = ERROR;
    CHAR    TaskName[M_TSKNAMELEN_A];
    CHAR    Func[] = "m1stream_Init";

    /* Copy profile content to module variables */
    m1stream_Debug = pConf->DebugMode;
    m1stream_CfgLine = pConf->LineNbr;
    m1stream_AppPrio = pConf->TskPrior;
    strncpy(m1stream_AppName, pConf->AppName, M_MODNAMELEN);
    m1stream_AppName[M_MODNAMELEN] = 0;
    strncpy(m1stream_ProfileName, pConf->ProfileName, M_PATHLEN);
    m1stream_ProfileName[M_PATHLEN] = 0;

    /* Conditional info message */
    LOG_I(2, Func, "Initializing module '%s' of type '%s.m' in partition %d",
    	  pConf->AppName, pConf->TypeName, pConf->MemPart);

    /* to be left upon error */
    do
    {
        /*
         * Deliver module parameters to resource management.
         * The variable m1stream_SmiId will be initialized by MOD.
         * This ID will be required as parameter to send and receive
         * SMI calls
         */
        if (res_ModParam(m1stream_AppName, M1STREAM_MINVERS,
                         M1STREAM_MAXVERS, RES_UNLIMITUSR, &m1stream_pSmiId) != RES_E_OK)
        {
            LOG_E(0, Func, "Could not register Module on MOD!");
            break;
        }

        /*
         * Base initialization of module
         * An error in this function does not abort initialization
         * but leads to the module state ERROR
         */
        if (BaseInit() < 0)
            m1stream_ModState = RES_S_ERROR;
        else
            m1stream_ModState = RES_S_EOI;

        /*
         * Start the SMI server as task for handling incoming SMI-calls.
         * This task should be in the priority group "Application 2"
         */
        snprintf(TaskName, sizeof(TaskName), "b%s", m1stream_AppName);
        TskId = sys_TaskSpawn(m1stream_AppName, TaskName, SMI_SRV_PRIO,
                              VX_FP_TASK, SMI_SRV_STACKSIZE, (FUNCPTR) bTaskMain);

        /* Test if task is successfully started */
        if (TskId == ERROR)
        {
            LOG_E(0, Func, "Error in sys_TaskSpawn;'%s'!", TaskName);
            break;
        }

        /*
         * After the values have been copied from pLoad, the memory can now be freed.
         * ATTENTION: In case of an error the module handler will free this memory.
         */
        if (pLoad->pCfg && pLoad->LenCfg)
            sys_MemPFree(pConf->MemPart, pLoad->pCfg);

        if (pLoad->pAttr && pLoad->LenAttr)
            sys_MemPFree(pConf->MemPart, pLoad->pAttr);

        /* Inform resource handler about changes of the module state */
        if (res_ModState(m1stream_AppName, m1stream_ModState) < 0)
            LOG_E(0, Func, "Change of Software-Module-State failed!");

        /* Success message */
        LOG_I(0, Func, "Initialized module '%s' of type '%s.m' in partition %d",
              pConf->AppName, pConf->TypeName, pConf->MemPart);

        /* Return Id of Task which will handle RPC-calls */
        return (TskId);

    }
    while (FALSE);

    /*
     * If processing reaches this point, there has been an error.
     * Free all resources which could have been allocated so far
     * and return an error.
     */
    LOG_E(0, Func, "Initialization error, cleaning up resources now");
    BaseDeinit();

    return (ERROR);
}

/**
********************************************************************************
* @brief Base initializations concerning the main task of the module
*        - reading configuration
*        - applying SVI
*        - start for module necessary tasks
*        - ...
*        Normally initialization routines of the adequate subranges of the
*        SW-module will be called.
*
* @param[in]  N/A
* @param[out] N/A
*
* @retval    >= 0 .. OK
* @retval     < 0 .. ERROR
*******************************************************************************/
MLOCAL SINT32 BaseInit(VOID)
{
    SINT32  ret = 0;
    SYM_TYPE symType = 0;
    CHAR    Func[] = "BaseInit";

    /*
     * Check availability of extended access functions
     * smi_Receive2 and svi_MsgHandler2 are newer versions with
     * improved security features. On MSys versions before 3.85,
     * the old functions must be used.
     * SINT32 smi_Receive(SMI_ID *SmiId, SMI_MSG *pMsg, SINT32 Timeout)
     * SINT32 smi_Receive2(SMI_ID *SmiId, SMI_MSG *pMsg, SINT32 Timeout , UINT32 *User);
     */
    symFindByName(sysSymTbl, "_smi_Receive2", (char **)&fpSmiReceive, &symType);
    if (!fpSmiReceive)
        symFindByName(sysSymTbl, "_smi_Receive", (char **)&fpSmiReceive, &symType);
    if (!fpSmiReceive)
    {
        LOG_E(0, Func, "Could not find smi_Receive function, SMI communication with module not possible!");
        LOG_E(0, Func, "Online de-installation of module not possible, reboot is necessary!");
    }

    symFindByName(sysSymTbl, "_svi_MsgHandler2", (char **)&fpSviMsgHandler, &symType);
    if (!fpSviMsgHandler)
        symFindByName(sysSymTbl, "_svi_MsgHandler", (char **)&fpSviMsgHandler, &symType);
    if (!fpSviMsgHandler)
        LOG_E(0, Func, "Could not find smi_MsgHandler function, external SVI access to module not possible!");

    /* create semaphore for halting all tasks in STOP state of module */
    if (!(m1stream_StateSema = semBCreate(SEM_Q_PRIORITY, SEM_EMPTY)))
    {
        LOG_E(0, Func, "Could not create semaphore for stop state of module tasks, aborting module initialization!");
        return (ERROR);
    }

    /*
     * Read configuration.
     * The SVI of the module can depend on the configuration,
     * so the configuration is read before the SVI server init is being called.
     */
    ret = m1stream_CfgRead();
    if (ret < 0)
    {
        LOG_E(0, Func, "Reading module configuration returned error, aborting module initialization!");
        return (ret);
    }

    /* Initialize SVI server */
    ret = m1stream_SviSrvInit();
    if (ret < 0)
    {
        LOG_E(0, Func, "Initializing SVI server for module returned error, aborting module initialization!");
        return (ret);
    }

    return (ret);
}

/**
********************************************************************************
* @brief Frees all module resources
*        Complement to BaseInit(), resources are free'd in reverse order.
*
* @param[in]  N/A
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID BaseDeinit(VOID)
{
    SINT32  ret;
    CHAR    Func[] = "BaseDeInit";

    /* Inform resource handler about changes of the module state */
    ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_DEINIT);
    if (ret != RES_E_OK)
        LOG_E(0, Func, "Change of Software-Module-State to DEINIT failed!");

    /* Delete semaphore for halting all tasks in STOP state of module */
    if (m1stream_StateSema)
    {
        if (semDelete(m1stream_StateSema) < 0)
            LOG_E(0, Func, "Could not delete state semaphore!");
        else
        	m1stream_StateSema = 0;
    }

    /*
     * Prevent external SVI-clients from accessing the module
     * before the exported resources are being deleted
     */
    m1stream_SviSrvDeinit();

    /* De-initialize resources allocated in m1stream_AppInit() */
    m1stream_AppDeinit();

}

/**
********************************************************************************
* @brief Started as communication task when the SW-module is loaded
*        Handles incoming SMI-calls in an endless loop.
*        Contains the entry point for the optional restart after an
*        exception (ExecpitonSignal) and for
*        optional shutdown sequences. (PanicSignal)
*
* @param[in]  N/A
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID bTaskMain(VOID)
{
    SINT32  Status;
    SMI_MSG Msg;
    SINT32  ret;
    UINT32  UserSessionId = 0;          /* Session Id for checking user rights */
    CHAR    Func[] = "bTaskMain";

    LOG_I(2, Func, "Starting communication task");

    /*
     * The function setjmp() acts as entry point after exceptions.
     * This is necessary if you want to restart you application
     * after an exception.
     */
    Status = setjmp(JumpEnv);
    if (Status == 0)
    {
        /* This branch will be taken after start of M1STREAM */

        /* Install signal handler for power-down */
        if (sys_PanicSigSet(PanicHandler) < 0)
            LOG_E(0, Func, "Panic signal handler could not be installed!");
    }
    else
    {
        /* This branch will be taken after longjmp() (after an exception) */
        LOG_I(2, Func, "Task restarted on signal %d.", Status);

        /* Cleanup after an exception */
        BaseDeinit();

        /* Make base initialization of module (after an exception) */
        if (BaseInit() < 0)
        {
            ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_ERROR);
            if (ret != RES_E_OK)
                LOG_E(0, Func, "Change of Software-Module-State to EOI failed!");
        }
        else
        {
            /* Module is now running correctly */
            ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_RUN);
            if (ret != RES_E_OK)
                LOG_E(0, Func, "Change of Software-Module-State to RUN failed!");
        }
    }

    /*
     * Initialization of SMI task is finished, the following endless loop
     * is executed endlessly as task for handling incoming SMI calls.
     * The task is suspended by the OS on calling the function smi_Receive().
     * smi_Receive can either WAIT_FOREVER (sleep until SMI call comes in),
     * or it can resume after the given timeout.
     * The timeout in smi_Receive() is only required if the software module
     * expects replies of other modules to own requests,
     * and these replies have to be checked for timeout.
     */
    while (1)
    {
        /*
         * Inform the system that the cycle ends.
         * This if for cycle time statistics,
         * values can be seen in the task view of the Device Manager.
         */
        sys_CycleEnd();

        /* Wait for SMI message in receive buffer */
        if (fpSmiReceive)
            Status = fpSmiReceive(m1stream_pSmiId, &Msg, WAIT_FOREVER, &UserSessionId);
        else
            Status = 0;

        /* Inform the system that the cycle starts */
        sys_CycleStart();

        /* Test if SMI message has been received */
        if (Status != 0)
        {
            continue;
        }

        if (Msg.Type & SMI_F_CALL)
        {
            switch (Msg.ProcRetCode)
            {
                case SMI_PROC_NULL:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_NULL", Func);
                    RpcNull(&Msg);
                    break;

                case SMI_PROC_DEINIT:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_DEINIT", Func);
                    RpcDeinit(&Msg);
                    return;             /* quit task completely in this case */

                case SMI_PROC_RESET:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_RESET", Func);
                    RpcReset(&Msg);
                    break;

                case SMI_PROC_STOP:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_STOP", Func);
                    RpcStop(&Msg);
                    break;

                case SMI_PROC_RUN:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_RUN", Func);
                    RpcRun(&Msg);
                    break;

                case SMI_PROC_NEWCFG:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_NEWCFG", Func);
                    RpcNewCfg(&Msg);
                    break;

                case SMI_PROC_GETINFO:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_GETINFO", Func);
                    RpcGetInfo(&Msg);
                    break;

                case SMI_PROC_ENDOFINIT:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_ENDOFINIT", Func);
                    RpcEndOfInit(&Msg);
                    break;

                case SMI_PROC_SETDBG:
                    LOG_I(4, m1stream_AppName, "%s: received call SMI_PROC_SETDBG", Func);
                    RpcSetDbg(&Msg);
                    break;

                    /*
                     * All SVI access operations that are required in SMI calls
                     * will be handled by the SVI handler.
                     */

                    /* for information on server and variable properties */
                case SVI_PROC_GETADDR:
                case SVI_PROC_GETPVINF:
                case SVI_PROC_GETSERVINF:
                    /* mainly used for list access access by SC and HMI */
                case SVI_PROC_GETVALLST:
                case SVI_PROC_SETVALLST:
                    /* mainly used by other applications for single access */
                case SVI_PROC_GETVAL:
                case SVI_PROC_SETVAL:
                case SVI_PROC_GETBLK:
                case SVI_PROC_SETBLK:
                case SVI_PROC_GETMULTIBLK:
                case SVI_PROC_SETMULTIBLK:
                    LOG_I(4, m1stream_AppName, "%s: received call SVI_PROC_....", Func);
                    /* Pass call to message handler */
                    if (fpSviMsgHandler)
                        fpSviMsgHandler(m1stream_SviHandle, &Msg, m1stream_pSmiId, UserSessionId);
                    break;

                    /* Not a standard SMI call */
                default:
                    LOG_W(2, m1stream_AppName, "%s: received unknown call with SMI id %d", Func, Msg.ProcRetCode);

                    smi_FreeData(&Msg);

                    if (smi_SendReply(m1stream_pSmiId, &Msg, SMI_E_PROC, 0, 0) < 0)
                        LOG_E(0, Func, "User defined smi_SendReply failed!");
            }
        }

        smi_FreeData(&Msg);
    }
}

/**
********************************************************************************
* @brief Handles the RPC-request SMI_PROC_NULL.
*
* @param[in]  pMsg    RPC--request
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcNull(SMI_MSG * pMsg)
{
    smi_FreeData(pMsg);
    if (smi_SendReply(m1stream_pSmiId, pMsg, SMI_E_OK, 0, 0) < 0)
        LOG_E(0, "RpcNull", "Case SMI_PROC_NULL - SMI SendReply failed!");
}

/**
********************************************************************************
* @brief Resets the module to the same state as after template_Init()
*
*        All module specific resources were freed and new allocated.
*
* @param[in]  pMsg    SMI call
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcReset(SMI_MSG * pMsg)
{
    SMI_RESET_R Reply;
    SINT32  ret;
    Reply.RetCode = SMI_E_OK;

    /* Freeing application specific resources */
    BaseDeinit();

    /* Make base initialization of module */
    if (BaseInit() < 0)
    {
        /* Set module state to ERROR */
        ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_ERROR);
        if (ret != RES_E_OK)
            LOG_E(0, "RpcReset", "Change of Software-Module-State to ERROR failed!");

        Reply.RetCode = SMI_E_FAILED;
    }
    else
    {
        /* Set module state to "End Of Init" */
        ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_EOI);
        if (ret != RES_E_OK)
            LOG_E(0, "RpcReset", "Change of Software-Module-State to EOI failed!");

        Reply.RetCode = SMI_E_OK;
    }

    /* Send reply */
    smi_FreeData(pMsg);
    if (smi_SendCReply(m1stream_pSmiId, pMsg, SMI_E_OK, &Reply, sizeof(Reply)) < 0)
        LOG_E(0, "RpcReset", "SMI SendCReply failed!");
}

/**
********************************************************************************
* @brief Sets the module from RUN to STOP state.
*        In this state only a few RPC's are accepted.
*
*        All additional tasks (beside m1stream_main()) have to implement the
*        following lines in order to stop the whole module:
*
*        if ((m1stream_ModState != RES_S_STOP) || (m1stream_ModState != RES_S_EOI))
*             semTake(m1stream_StateSema, WAIT_FOREVER);
*
* @param[in]  pMsg    RPC-request
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcStop(SMI_MSG * pMsg)
{
    SMI_STOP_R Reply;
    SINT32  ret;

    if (m1stream_ModState != RES_S_RUN)
    {
        Reply.RetCode = SMI_E_FAILED;
    }
    else
    {
        /* Set module state */
        ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_STOP);
        if (ret != RES_E_OK)
            LOG_E(0, "RpcStop", "Change of Software-Module-State to STOP failed!");

        Reply.RetCode = SMI_E_OK;
    }

    /* Send reply */
    smi_FreeData(pMsg);
    if (smi_SendCReply(m1stream_pSmiId, pMsg, SMI_E_OK, &Reply, sizeof(Reply)) < 0)
        LOG_E(0, "RpcStop", "SMI SendCReply failed!");
}

/**
********************************************************************************
* @brief Changes module state from STOP to RUN
*
* @param[in]  pMsg    RPC-request
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcRun(SMI_MSG * pMsg)
{
    SMI_RUN_R Reply;
    SINT32  ret;

    if (m1stream_ModState != RES_S_STOP)
    {
        LOG_E(0, "RpcRun", "Module is not in STOP state!");
        Reply.RetCode = SMI_E_FAILED;
    }
    else
    {
        /* Set module state to RUN */
        ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_RUN);
        if (ret != RES_E_OK)
            LOG_E(0, "RpcRun", "Change of Software-Module-State to RUN failed!");

        /* Restart all stopped tasks of the module which are waiting in semTake() */
        semFlush(m1stream_StateSema);

        Reply.RetCode = SMI_E_OK;
    }

    /* Send reply */
    smi_FreeData(pMsg);
    if (smi_SendCReply(m1stream_pSmiId, pMsg, SMI_E_OK, &Reply, sizeof(Reply)) < 0)
        LOG_E(0, "RpcRun", "SMI SendCReply failed!");
}

/**
********************************************************************************
* @brief Reloads the module configuration.
*
* @param[in]  pMsg    SMI call
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcNewCfg(SMI_MSG * pMsg)
{
    SMI_NEWCFG_R Reply;
    SINT32  ret;

    /* Test if module is in a valid state to take over a new configuration */
    if (m1stream_ModState == RES_S_STOP || m1stream_ModState == RES_S_RUN ||
        m1stream_ModState == RES_S_ERROR)
    {
        /* Remove application (if it is running) */
        m1stream_AppDeinit();

        /* Restart application with the new configuration */
        if (m1stream_CfgRead() || m1stream_AppEOI())
        {
            ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_ERROR);
            if (ret != RES_E_OK)
                LOG_E(0, "RpcNewCfg", "Change of Software-Module-State to ERROR failed!");

            Reply.RetCode = SMI_E_FAILED;
        }
        else
        {
            /* Set module state OK */
            ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_EOI);
            if (ret != RES_E_OK)
                LOG_E(0, "RpcNewCfg", "Change of Software-Module-State to EOI failed!");

            Reply.RetCode = SMI_E_OK;
        }
    }
    else
        Reply.RetCode = SMI_E_FAILED;

    /* Send reply */
    smi_FreeData(pMsg);
    if (smi_SendCReply(m1stream_pSmiId, pMsg, SMI_E_OK, &Reply, sizeof(Reply)) < 0)
        LOG_E(0, "RpcNewCfg", "SMI SendCReply failed!");
}

/**
********************************************************************************
* @brief Handles the RPC-call SMI_PROC_DEINIT.
*        This RPC-call causes the module to delete itself.
*
* @param[in]  pMsg    SMI call
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcDeinit(SMI_MSG * pMsg)
{
    SMI_DEINIT_R Reply;

    /*
     * First the reply-message has to be sent back,
     * because m1stream_SmiId will become invalid
     * after calling res_ModDelete().
     */
    Reply.RetCode = SMI_E_OK;
    smi_FreeData(pMsg);
    if (smi_SendCReply(m1stream_pSmiId, pMsg, SMI_E_OK, &Reply, sizeof(Reply)) < 0)
        LOG_E(0, "RpcDeinit", "SMI SendCReply failed!");

    /* Freeing application specific resources */
    BaseDeinit();

    /* Logout from the resource handler */
    if (res_ModDelete(m1stream_AppName) != RES_E_OK)
        LOG_E(0, "RpcDeinit", "Delete of module resource failed!");

    /* De-install signal handlers */
    if (sys_PanicSigReset() < 0)
        LOG_E(0, "RpcDeinit", "Panic signal handler could not be deleted!",
              m1stream_AppName);

    if (sys_ExcSigReset() < 0)
        LOG_E(0, "RpcDeinit", "Exception signal handler could not be deleted!");

    LOG_I(1, "RpcDeinit", "Module has been deleted");
}

/**
********************************************************************************
* @brief Handles the SMI call SMI_PROC_ENDOFINIT.
*        This call causes the module to make all initializations which
*        are required by the module and sets it to the state RUN.
*
*        A operational system requires that all SW-modules are started and
*        had initialized there environment interfaces.
*
* @param[in]  pMsg    SMI call
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcEndOfInit(SMI_MSG * pMsg)
{
    SMI_ENDOFINIT_R Reply;
    SINT32  ret;
    CHAR	Func[]="RpcEndOfInit";

    Reply.RetCode = SMI_E_OK;

    if (m1stream_ModState != RES_S_EOI)
    {
        LOG_E(0, Func, "Module is not in end of init state!");

        /* Wrong module state for "End of Init" */
        Reply.RetCode = SMI_E_FAILED;

        /* Send reply and return */
        smi_FreeData(pMsg);
        if (smi_SendCReply(m1stream_pSmiId, pMsg, SMI_E_OK, &Reply, sizeof(Reply)) < 0)
            LOG_E(0, Func, "SMI SendCReply failed!");

        return;
    }

    /* Installing my application task */
    if (m1stream_AppEOI() < 0)
    {
        Reply.RetCode = SMI_E_FAILED;
    }
    else
    {
        /* Module is now running correctly */
        ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_RUN);

        if (ret)
        {
            LOG_E(0, Func, "Change of Software-Module-State to RUN failed!");
            Reply.RetCode = SMI_E_FAILED;
        }
        else
        {
            /* Restart all stopped tasks of the module which are waiting in semTake() */
            semFlush(m1stream_StateSema);

            LOG_I(1, Func, "Module successfully started.");
        }
    }

    if (Reply.RetCode == SMI_E_FAILED)
    {
        ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_ERROR);
        if (ret != RES_E_OK)
            LOG_E(0, Func, "Change of module state to ERROR failed!");
    }
    else
    {
        ret = res_ModState(m1stream_AppName, m1stream_ModState = RES_S_RUN);
        if (ret != RES_E_OK)
            LOG_E(0, Func, "Change of module state to RUN failed!");
    }

    /* Send reply */
    smi_FreeData(pMsg);
    if (smi_SendCReply(m1stream_pSmiId, pMsg, SMI_E_OK, &Reply, sizeof(Reply)) < 0)
        LOG_E(0, Func, "SMI SendCReply failed!");
}

/**
********************************************************************************
* @brief Handles the RPC-request SMI_PROC_SETDBG.
*        This RPC-call causes the module to set the debug mode.
*
* @param[in]  pMsg    RPC-request
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcSetDbg(SMI_MSG * pMsg)
{
    SMI_SETDBG_C *pCall;
    SMI_SETDBG_R Reply;

    pCall = (SMI_SETDBG_C *) pMsg->Data;

    /* Take over the new debug mode */
    m1stream_Debug = pCall->DebugMode;
    Reply.RetCode = SMI_E_OK;

    /* Send reply */
    smi_FreeData(pMsg);
    if (smi_SendCReply(m1stream_pSmiId, pMsg, SMI_E_OK, &Reply, sizeof(Reply)) < 0)
        LOG_E(0, "RpcSetDbg", "SMI SendCReply failed!");
}

/**
********************************************************************************
* @brief Handles the RPC-request SMI_PROC_GETINFO.
*
* @param[in]  pMsg    RPC-request
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID RpcGetInfo(SMI_MSG * pMsg)
{
    SMI_GETINFO_R *pReply;
    SYS_VERSION Version;

    smi_FreeData(pMsg);

    /* Allocate memory for the answer */
    pReply = smi_MemAlloc(sizeof(*pReply));
    if (!pReply)
    {
        LOG_E(0, "RpcGetInfo", "No memory!");
        if (smi_SendReply(m1stream_pSmiId, pMsg, SMI_E_ARGS, 0, 0) < 0)
            LOG_E(0, "RpcGetInfo", "SendReply failed!");

        return;
    }

    /* Deposit module name and description */
    strncpy(pReply->Name, m1stream_AppName, M_MODNAMELEN);
    pReply->Name[M_MODNAMELEN] = 0;

    strncpy(pReply->Desc, m1stream_ModuleInfoDesc, sizeof(pReply->Desc));
    pReply->Desc[sizeof(pReply->Desc) - 1] = 0;

    /*
     * Convert the version string to an integer
     * CONSIDERATION:   The version string has the following format
     *                  "Vaa.bb.cc ddd"  whereas cc is optional
     *                  Example: "V1.02.03 Alpha", "V2.12 Beta",
     *                           "V5.01 Release" or "V5.01"
     * In this case, the msys function sys_GetVersion() can be used.
     */
    sys_GetVersion(&Version, m1stream_Version);
    pReply->VersType = Version.Type;
    memcpy(&pReply->VersCode, Version.Code, sizeof(pReply->VersCode));

    pReply->State = m1stream_ModState;
    pReply->DebugMode = m1stream_Debug;
    pReply->RetCode = SMI_E_OK;

    /* Send reply */
    if (smi_SendReply(m1stream_pSmiId, pMsg, SMI_E_OK, pReply, sizeof(*pReply)) < 0)
        LOG_E(0, "RpcGetInfo", "SendReply of module information (Ping) failed!");
}

/**
********************************************************************************
* @brief Handler for panic-situation.
*
* @param[in]  PanicMode       Type of panic-situation (SYS_APPPANIC, ...)
* @param[out] N/A
*
* @retval     N/A
*******************************************************************************/
MLOCAL VOID PanicHandler(UINT32 PanicMode)
{
    /*
     * TODO:
     * Bring critical parts to a predefined state.
     * For example save data to NV-RAM or close open files.
     */
}
