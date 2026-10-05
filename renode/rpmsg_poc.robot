*** Comments ***
Acceptance test for the AM64x RPMsg POC. Run from the repository root:
    renode-test renode/rpmsg_poc.robot

*** Settings ***
# Each test needs a fresh boot: the R5F handles one RPMsg session per boot
Test Setup                  Reset Emulation

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

Boot And Log In
    ${linux}  ${zephyr}=        Create POC Machine
    Wait For Line On Uart       OpenAMP: waiting for the A53 to initialize the virtio device    testerId=${zephyr}
    Wait For Prompt On Uart     buildroot login:            testerId=${linux}   timeout=300
    Write Line To Uart          root                        testerId=${linux}
    Wait For Prompt On Uart     ${LINUX_PROMPT}             testerId=${linux}
    RETURN                      ${linux}  ${zephyr}

*** Test Cases ***
Should Exchange RPMsg Between A53 Linux And R5F Zephyr
    ${linux}  ${zephyr}=        Boot And Log In

    Write Line To Uart          ${APP} ping                 testerId=${linux}
    Wait For Line On Uart       am64_rpmsg_userspace: sent "ping"                   testerId=${linux}   timeout=120
    Wait For Line On Uart       OpenAMP: Received message: "ping"                   testerId=${zephyr}  timeout=60
    Wait For Line On Uart       am64_rpmsg_userspace: received "pong"               testerId=${linux}   timeout=60
    Wait For Line On Uart       am64_rpmsg_userspace: done                          testerId=${linux}   timeout=60

Should Exchange Custom Messages In Both Directions
    ${linux}  ${zephyr}=        Boot And Log In

    Write Line To Uart          ${APP}                      testerId=${linux}
    Wait For Line On Uart       OpenAMP: A53 connected                              testerId=${zephyr}  timeout=120
    Wait For Line On Uart       am64_rpmsg_userspace: interactive mode              testerId=${linux}   timeout=60

    # Linux -> Zephyr, no automatic reply
    Write Line To Uart          hello from linux            testerId=${linux}
    Wait For Line On Uart       am64_rpmsg_userspace: sent "hello from linux"       testerId=${linux}   timeout=60
    Wait For Line On Uart       OpenAMP: Received message: "hello from linux"       testerId=${zephyr}  timeout=60
    Should Not Be On Uart       OpenAMP: Sent reply         testerId=${zephyr}  timeout=2

    # Zephyr -> Linux
    Write Line To Uart          rpmsg send hello from zephyr    testerId=${zephyr}
    Wait For Line On Uart       OpenAMP: Sent message: "hello from zephyr"          testerId=${zephyr}  timeout=60
    Wait For Line On Uart       am64_rpmsg_userspace: received "hello from zephyr"  testerId=${linux}   timeout=60

    # ping -> pong still works mid-session
    Write Line To Uart          ping                        testerId=${linux}
    Wait For Line On Uart       OpenAMP: Received message: "ping"                   testerId=${zephyr}  timeout=60
    Wait For Line On Uart       am64_rpmsg_userspace: received "pong"               testerId=${linux}   timeout=60

    Write Line To Uart          quit                        testerId=${linux}
    Wait For Line On Uart       am64_rpmsg_userspace: done                          testerId=${linux}   timeout=60

Should Transfer Bulk Payloads In Both Directions
    ${linux}  ${zephyr}=        Boot And Log In

    Write Line To Uart          dd if=/dev/urandom of=/tmp/blob bs=1M count=1    testerId=${linux}
    Wait For Prompt On Uart     ${LINUX_PROMPT}             testerId=${linux}   timeout=60

    Write Line To Uart          ${APP}                      testerId=${linux}
    Wait For Line On Uart       OpenAMP: A53 bulk channel connected                 testerId=${zephyr}  timeout=120
    Wait For Line On Uart       am64_rpmsg_userspace: interactive mode              testerId=${linux}   timeout=60

    # Linux -> Zephyr: 1 MB file through the bulk region, checked in place by the R5F
    Write Line To Uart          /bulk /tmp/blob             testerId=${linux}
    Wait For Line On Uart       am64_rpmsg_userspace: sent bulk 1: 1048576 bytes    testerId=${linux}   timeout=120
    Wait For Line On Uart       OpenAMP: Received bulk 1: 1048576 bytes, crc 0x[0-9a-f]{8} OK    testerId=${zephyr}  timeout=120  treatAsRegex=true
    Wait For Line On Uart       am64_rpmsg_userspace: bulk 1 released               testerId=${linux}   timeout=60

    # The payload can be viewed in place on the R5F
    Write Line To Uart          rpmsg dump 32               testerId=${zephyr}
    Wait For Line On Uart       OpenAMP: bulk 1, bytes 0..31 of 1048576:            testerId=${zephyr}  timeout=60
    Wait For Line On Uart       00000010:                   testerId=${zephyr}  timeout=60

    # Zephyr -> Linux: 2 MB test pattern
    Write Line To Uart          rpmsg bulk 2M               testerId=${zephyr}
    Wait For Line On Uart       OpenAMP: Sent bulk 1: 2097152 bytes                 testerId=${zephyr}  timeout=120
    Wait For Line On Uart       am64_rpmsg_userspace: received bulk 1: 2097152 bytes, crc 0x[0-9a-f]{8} OK    testerId=${linux}   timeout=120  treatAsRegex=true
    Wait For Line On Uart       am64_rpmsg_userspace: saved bulk 1 to /tmp/bulk_1.bin    testerId=${linux}   timeout=60
    Wait For Line On Uart       OpenAMP: bulk 1 released                            testerId=${zephyr}  timeout=60

    # Text messages still work alongside
    Write Line To Uart          ping                        testerId=${linux}
    Wait For Line On Uart       am64_rpmsg_userspace: received "pong"               testerId=${linux}   timeout=60

    Write Line To Uart          quit                        testerId=${linux}
    Wait For Line On Uart       am64_rpmsg_userspace: done                          testerId=${linux}   timeout=60

    # The saved file holds Zephyr's pattern: byte i = i * 31 + id
    Wait For Prompt On Uart     ${LINUX_PROMPT}             testerId=${linux}   timeout=60
    Write Line To Uart          head -c 16 /tmp/bulk_1.bin | hexdump -C    testerId=${linux}
    Wait For Line On Uart       00000000${SPACE}${SPACE}01 20 3f 5e 7d 9c bb da${SPACE}${SPACE}f9 18 37 56 75 94 b3 d2   testerId=${linux}   timeout=60
