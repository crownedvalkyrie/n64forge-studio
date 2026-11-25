#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>

class QMovie;

namespace n64forge::studio {

// Renders the Valkyrie mark used by the splash screen and window/dock icon.
[[nodiscard]] QPixmap valkyrieIcon(int size);

class SplashDialog : public QDialog {
    Q_OBJECT
public:
    explicit SplashDialog(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private slots:
    void tick();

private:
    QTimer* timer_ = nullptr;
    QMovie* gif_ = nullptr;
    QElapsedTimer elapsed_;
    static constexpr int DURATION_MS = 3400;
};

} // namespace n64forge::studio