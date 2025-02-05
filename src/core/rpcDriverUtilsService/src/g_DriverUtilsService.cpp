/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include <g_DriverUtilsService.h>

namespace DriverUtils
{

const DDRpcServerRegisterServiceInfo IDriverUtilsService::kServiceInfo = []() -> DDRpcServerRegisterServiceInfo {
    DDRpcServerRegisterServiceInfo info = {};
    info.id                             = 0x24815012;
    info.version.major                  = 1;
    info.version.minor                  = 0;
    info.version.patch                  = 0;
    info.pName                          = "DriverUtils";
    info.pDescription                   = "A utilities service for modifying the driver.";

    return info;
}();

static DD_RESULT RegisterFunctions(
    DDRpcServer hServer,
    IDriverUtilsService* pService)
{
    DD_RESULT result = DD_RESULT_SUCCESS;

    // Register "EnableTracing"
    if (result == DD_RESULT_SUCCESS)
    {
        DDRpcServerRegisterFunctionInfo info = {};
        info.serviceId                       = 0x24815012;
        info.id                              = 0x1;
        info.pName                           = "EnableTracing";
        info.pDescription                    = "Informs driver we are collecting trace data";
        info.pFuncUserdata                   = pService;
        info.pfnFuncCb                       = [](
            const DDRpcServerCallInfo* pCall) -> DD_RESULT
        {
            auto* pService = reinterpret_cast<IDriverUtilsService*>(pCall->pUserdata);

            // Execute the service implementation
            return pService->EnableTracing();
        };

        result = ddRpcServerRegisterFunction(hServer, &info);
    }

    return result;
}

DD_RESULT RegisterService(
    DDRpcServer hServer,
    IDriverUtilsService* pService
)
{
    // Register the service
    DD_RESULT result = ddRpcServerRegisterService(hServer, &IDriverUtilsService::kServiceInfo);

    // Register individual functions
    if (result == DD_RESULT_SUCCESS)
    {
        result = RegisterFunctions(hServer, pService);

        if (result != DD_RESULT_SUCCESS)
        {
            // Unregister the service if registering functions fails
            ddRpcServerUnregisterService(hServer, IDriverUtilsService::kServiceInfo.id);
        }
    }

    return result;
}

} // namespace DriverUtils
