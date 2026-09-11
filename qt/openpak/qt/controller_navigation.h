// SPDX-License-Identifier: GPL-2.0-or-later
// Gamepad navigation for the dialogs. The host subclasses this and emits from its own input
// stack (Citron: HID core); a bare instance is a valid "no gamepad" implementation.
#pragma once
#include <QObject>
namespace openpak::qt {
class Navigation : public QObject {
    Q_OBJECT
public:
    explicit Navigation(QObject* parent = nullptr) : QObject(parent) {}
signals:
    void navigated(int dx, int dy);
    void activated();      // A
    void cancelled();      // B
    void backPressed();    // B, as the dialogs name it
    void leftShoulderPressed();
    void rightShoulderPressed();
    void auxiliaryAction(int action_id); // X, Y, ...
    void activityDetected();
};
} // namespace openpak::qt
