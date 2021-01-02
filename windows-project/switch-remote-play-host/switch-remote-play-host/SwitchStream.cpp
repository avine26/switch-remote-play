#include "SwitchStream.h"

#include <processthreadsapi.h>
#include "SwitchControlsDefinitions.h"
#include "VirtualController.h"
#include "Connection.h"
#include "FFMPEGHelper.h"
#include <chrono>

CommandPayload ReadPayloadFromSwitch(SOCKET const& switchSocket)
{
    using namespace std;

    cout << "Expected size of command payload: " << COMMAND_PAYLOAD_SIZE << " bytes" << endl;
    
    CommandPayload data{};
    char* readBuffer = (char*)(&data);
    int retryCount = 3;

    cout << "Reading command..." << endl;
    
    do
    {
        auto result = recv(switchSocket, readBuffer, COMMAND_PAYLOAD_SIZE, 0);
        if (result == SOCKET_ERROR)
        {
            cout << "Failed to receive data" << endl;
            --retryCount;
            data.commandCode = retryCount <= 0 ? Command::SHUTDOWN : Command::IGNORE_COMMAND;
        }
        else if (result == 0) //once the switch closes the command socket, this will hit
        {
            cout << "breaking as switch closed connection" << endl;
            return data;
        }
        else
            return data;
    } while (retryCount > 0);

    return data;
}

std::thread StartGamepadListener(int16_t mouseSensitivity, std::atomic_bool& killStream, std::atomic_bool& gamepadActive, uint16_t port)
{
    using namespace std;
    thread workerThread{};

    if (gamepadActive.load(memory_order_acquire))
    {
        gamepadActive.store(false, memory_order_release);
    }

    workerThread = thread([&, gamepadPort=port] {

        while (gamepadActive.load(memory_order_acquire)) //don't let it continue if a previous gamepadThread is running
        {
            this_thread::sleep_for(chrono::duration<int, milli>(100)); //wait a bit to let previous gamepad thread cleanup
        }

        auto connection = Connection(gamepadPort);
        if(connection.Ready())
        {
            if(connection.WaitForConnection())
            {
                //can use the switch socket now to listen for input sent over
                GamepadDataPayload padData = {};
                for (auto& c : padData.padding)
                    c = 0;
                VirtualController controller{};
                if (controller.Create())
                {
                    auto streamDead = killStream.load(memory_order_acquire);
                    gamepadActive.store(true, memory_order_release);
                    auto active = true;
                    auto const maxRetries = 5;
                    auto retryCount = maxRetries;

                    auto constexpr mouseToggleNano = 3000000000;
                    auto lastTime = std::chrono::high_resolution_clock::now();
                    auto mouseMode{ true };
                    auto constexpr mouseToggleBtnCombo = KEY_ZL | KEY_ZR | KEY_B;
                    auto mouseBtnFlags = 0;
                    double constexpr joystickExtent = 0xFFFF / 2;
                    do
                    {
                        active = gamepadActive.load(memory_order_acquire);
                        padData.keys = 0;
                        padData.ljx = padData.ljy = 0;
                        padData.rjx = padData.rjy = 0;
                        
                        auto result = recv(connection.ConnectedSocket(), (char*)&padData, GamepadDataPayloadSize, 0);
                        if (result == SOCKET_ERROR)
                        {
                            cout << "Failed to receive data for gamepad stream" << endl;
                            std::this_thread::sleep_for(std::chrono::duration<int, std::milli>(1000));
                            --retryCount;
                            if (retryCount == 0)
                            {
                                killStream.store(true, memory_order_release);
                                active = false;
                                streamDead = true;
                            }
                        }
                        else if (result > 0)
                        {
                            if (padData.keys == 0xFFFF)
                            {
                                cout << "Received kill signal" << endl;
                                killStream.store(true, memory_order_release);
                                active = false;
                                streamDead = true;
                            }
                            else if (mouseMode)
                            {
                                INPUT mouseInput{ 0 };
                                mouseInput.type = INPUT_MOUSE;
                                mouseInput.mi.dwFlags = MOUSEEVENTF_MOVE;
                                auto x = (padData.rjx + padData.ljx) % (int)joystickExtent;
                                auto y = (-padData.rjy + -padData.ljy) % (int)joystickExtent;
                                mouseInput.mi.dx = (long)(mouseSensitivity * (x / joystickExtent));
                                mouseInput.mi.dy = (long)(mouseSensitivity * (y / joystickExtent));

                                // left mouse button
                                if (padData.keys & KEY_L && !(mouseBtnFlags & MOUSEEVENTF_LEFTDOWN))
                                {
                                    mouseInput.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
                                    mouseBtnFlags |= MOUSEEVENTF_LEFTDOWN;
                                }
                                else if (!(padData.keys & KEY_L) && (mouseBtnFlags & MOUSEEVENTF_LEFTDOWN))
                                {
                                    mouseInput.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
                                    mouseBtnFlags &= ~MOUSEEVENTF_LEFTDOWN;
                                }

                                // right mouse button
                                if (padData.keys & KEY_R && !(mouseBtnFlags & MOUSEEVENTF_RIGHTDOWN))
                                {
                                    mouseInput.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
                                    mouseBtnFlags |= MOUSEEVENTF_RIGHTDOWN;
                                }
                                else if (!(padData.keys & KEY_R) && (mouseBtnFlags & MOUSEEVENTF_RIGHTDOWN))
                                {
                                    mouseInput.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
                                    mouseBtnFlags &= ~MOUSEEVENTF_RIGHTDOWN;
                                }
                                SendInput(1, &mouseInput, sizeof(INPUT));
                            }
                            else if (padData.keys != 0x0 || (padData.ljx | padData.ljy | padData.rjx | padData.rjy) != 0)
                            {
                                //update controller
                                auto input = ConvertToXUSB(padData);
                                PrintXUSBConversion(input);
                                controller.SetButtons(input.buttons);
                                controller.SetAnalogAxis(input.lx, input.ly, input.rx, input.ry);
                                controller.SetShoulderTriggers(input.lt, input.rt);

                                controller.UpdateController();
                            }
                            else
                            {
                                controller.ResetController();
                                controller.UpdateController();
                            }

                            // figure out if we should toggle mouse mode
                            if (padData.keys == mouseToggleBtnCombo || padData.keys & KEY_TOUCH)
                            {
                                auto timePassed = std::chrono::high_resolution_clock::now() - lastTime;
                                if (timePassed.count() >= mouseToggleNano)
                                {
                                    controller.ResetController();
                                    controller.UpdateController();

                                    mouseMode = !mouseMode;

                                    if (mouseMode)
                                        MessageBeep(MB_OK);
                                    else
                                        MessageBeep(MB_ICONERROR);

                                    if (mouseBtnFlags & (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN))
                                    {
                                        INPUT mouseInput{ 0 };
                                        mouseInput.type = INPUT_MOUSE;
                                        if(mouseBtnFlags & MOUSEEVENTF_LEFTDOWN)
                                            mouseInput.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
                                        if (mouseBtnFlags & MOUSEEVENTF_RIGHTDOWN)
                                            mouseInput.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
                                        mouseBtnFlags &= ~MOUSEEVENTF_LEFTDOWN;
                                        mouseBtnFlags &= ~MOUSEEVENTF_RIGHTDOWN;

                                        SendInput(1, &mouseInput, sizeof(INPUT));
                                    }

                                    lastTime = std::chrono::high_resolution_clock::now();
                                }
                            }
                            else
                                lastTime = std::chrono::high_resolution_clock::now();
                        }
                        else if (result == 0)
                        {
                            // connection closed, cleanup
                            killStream.store(true, memory_order_release);
                            active = false;
                            streamDead = true;
                        }
                    } while (padData.keys != 0xFFFF && !streamDead && active);

                    // make sure there aren't accidental junk key presses when stream dies
                    if (mouseBtnFlags & (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN))
                    {
                        INPUT mouseInput{ 0 };
                        mouseInput.type = INPUT_MOUSE;
                        if (mouseBtnFlags & MOUSEEVENTF_LEFTDOWN)
                            mouseInput.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
                        if (mouseBtnFlags & MOUSEEVENTF_RIGHTDOWN)
                            mouseInput.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
                        mouseBtnFlags &= ~MOUSEEVENTF_LEFTDOWN;
                        mouseBtnFlags &= ~MOUSEEVENTF_RIGHTDOWN;

                        SendInput(1, &mouseInput, sizeof(INPUT));
                    }

                    controller.ResetController();
                    controller.UpdateController();
                    controller.Disconnect();
                }
            }
        }

        connection.Close();

        killStream.store(true, memory_order_release);
        gamepadActive.store(false, memory_order_release);
    });

    return workerThread;
}
