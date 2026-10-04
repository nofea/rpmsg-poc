*** Comments ***
Acceptance test for the AM64x RPMsg POC. Run from the repository root:
    renode-test renode/rpmsg_poc.robot

*** Variables ***
${LINUX_UART}       sysbus.uart1
${ZEPHYR_UART}      sysbus.uart0
${LINUX_PROMPT}     \#${SPACE}
${APP}              /root/am64_rpmsg_userspace

*** Keywords ***
Create POC Machine
    Execute Command             path add @${CURDIR}/..
    Execute Command             include @renode/run_poc.resc
    ${linux}=                   Create Terminal Tester      ${LINUX_UART}   defaultPauseEmulation=true
    ${zephyr}=                  Create Terminal Tester      ${ZEPHYR_UART}  defaultPauseEmulation=true
    RETURN                      ${linux}  ${zephyr}

*** Test Cases ***
Should Exchange RPMsg Between A53 Linux And R5F Zephyr
    ${linux}  ${zephyr}=        Create POC Machine

    Wait For Line On Uart       OpenAMP: waiting for the A53 to initialize the virtio device    testerId=${zephyr}

    Wait For Prompt On Uart     buildroot login:            testerId=${linux}   timeout=300
    Write Line To Uart          root                        testerId=${linux}
    Wait For Prompt On Uart     ${LINUX_PROMPT}             testerId=${linux}

    Write Line To Uart          ${APP}                      testerId=${linux}
    Wait For Line On Uart       am64_rpmsg_userspace: sent "ping"                   testerId=${linux}   timeout=120
    Wait For Line On Uart       OpenAMP: Received message: "ping"                   testerId=${zephyr}  timeout=60
    Wait For Line On Uart       am64_rpmsg_userspace: received "pong"               testerId=${linux}   timeout=60
    Wait For Line On Uart       am64_rpmsg_userspace: done                          testerId=${linux}   timeout=60
