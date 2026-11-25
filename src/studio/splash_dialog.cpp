#include "n64forge/studio/splash_dialog.hpp"

#include <QApplication>
#include <QDir>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMovie>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QScreen>
#include <QVector>
#include <QtMath>

namespace n64forge::studio {

namespace {

void drawValkyrieGlyph(QPainter& painter, const QRect& rect) {
    // Metallic silver/gold palette.
    QColor silver(220, 225, 235);
    QColor gold(215, 175, 70);
    QColor shadow(60, 60, 75);

    const QPointF center = rect.center();
    const qreal w = rect.width();
    const qreal h = rect.height();

    // Wing left.
    QPainterPath leftWing;
    leftWing.moveTo(center.x() - w * 0.18, center.y() - h * 0.05);
    leftWing.cubicTo(center.x() - w * 0.55, center.y() - h * 0.45,
                     center.x() - w * 0.55, center.y() + h * 0.05,
                     center.x() - w * 0.22, center.y() + h * 0.18);
    leftWing.cubicTo(center.x() - w * 0.35, center.y() + h * 0.05,
                     center.x() - w * 0.35, center.y() - h * 0.15,
                     center.x() - w * 0.18, center.y() - h * 0.05);
    QLinearGradient leftWingGrad(center.x() - w * 0.20, center.y(),
                                 center.x() - w * 0.55, center.y());
    leftWingGrad.setColorAt(0.0, silver);
    leftWingGrad.setColorAt(1.0, QColor(180, 188, 205));
    painter.fillPath(leftWing, leftWingGrad);
    painter.setPen(QPen(shadow, 2));
    painter.drawPath(leftWing);

    // Wing right.
    QPainterPath rightWing;
    rightWing.moveTo(center.x() + w * 0.18, center.y() - h * 0.05);
    rightWing.cubicTo(center.x() + w * 0.55, center.y() - h * 0.45,
                      center.x() + w * 0.55, center.y() + h * 0.05,
                      center.x() + w * 0.22, center.y() + h * 0.18);
    rightWing.cubicTo(center.x() + w * 0.35, center.y() + h * 0.05,
                      center.x() + w * 0.35, center.y() - h * 0.15,
                      center.x() + w * 0.18, center.y() - h * 0.05);
    QLinearGradient rightWingGrad(center.x() + w * 0.20, center.y(),
                                  center.x() + w * 0.55, center.y());
    rightWingGrad.setColorAt(0.0, silver);
    rightWingGrad.setColorAt(1.0, QColor(180, 188, 205));
    painter.fillPath(rightWing, rightWingGrad);
    painter.setPen(QPen(shadow, 2));
    painter.drawPath(rightWing);

    // Helmet dome.
    QPainterPath helmet;
    helmet.moveTo(center.x(), center.y() - h * 0.42);
    helmet.cubicTo(center.x() - w * 0.35, center.y() - h * 0.42,
                   center.x() - w * 0.32, center.y() + h * 0.18,
                   center.x(), center.y() + h * 0.28);
    helmet.cubicTo(center.x() + w * 0.32, center.y() + h * 0.18,
                   center.x() + w * 0.35, center.y() - h * 0.42,
                   center.x(), center.y() - h * 0.42);
    QLinearGradient helmetGrad(center.x(), center.y() - h * 0.45,
                               center.x(), center.y() + h * 0.25);
    helmetGrad.setColorAt(0.0, silver);
    helmetGrad.setColorAt(1.0, shadow.lighter(140));
    painter.fillPath(helmet, helmetGrad);
    painter.setPen(QPen(gold, 3));
    painter.drawPath(helmet);

    // Eye slit / visor.
    QPainterPath visor;
    visor.moveTo(center.x() - w * 0.22, center.y() - h * 0.05);
    visor.quadTo(center.x(), center.y() + h * 0.12, center.x() + w * 0.22, center.y() - h * 0.05);
    visor.quadTo(center.x(), center.y() + h * 0.02, center.x() - w * 0.22, center.y() - h * 0.05);
    QLinearGradient visorGrad(center.x(), center.y() - h * 0.06,
                              center.x(), center.y() + h * 0.12);
    visorGrad.setColorAt(0.0, QColor(190, 245, 255, 210));
    visorGrad.setColorAt(1.0, QColor(70, 170, 210, 160));
    painter.fillPath(visor, visorGrad);
    painter.setPen(QPen(gold, 1));
    painter.drawPath(visor);

    // Center crest.
    QPainterPath crest;
    crest.moveTo(center.x(), center.y() - h * 0.44);
    crest.cubicTo(center.x() - w * 0.05, center.y() - h * 0.30,
                  center.x() - w * 0.07, center.y() - h * 0.10,
                  center.x() - w * 0.02, center.y() + h * 0.02);
    crest.lineTo(center.x() + w * 0.02, center.y() + h * 0.02);
    crest.cubicTo(center.x() + w * 0.07, center.y() - h * 0.10,
                  center.x() + w * 0.05, center.y() - h * 0.30,
                  center.x(), center.y() - h * 0.44);
    painter.fillPath(crest, QBrush(gold));
}

qreal clamp01(qreal x) {
    return qBound(0.0, x, 1.0);
}

qreal easeOutCubic(qreal x) {
    const qreal y = clamp01(x);
    return 1.0 - std::pow(1.0 - y, 3.0);
}

qreal easeOutBack(qreal x) {
    const qreal y = clamp01(x);
    const qreal c1 = 1.70158;
    const qreal c3 = c1 + 1.0;
    return 1.0 + c3 * std::pow(y - 1.0, 3.0) + c1 * std::pow(y - 1.0, 2.0);
}

// Progress of `ms` inside the [start, start + duration) window, 0..1.
qreal windowed(qreal ms, qreal start, qreal duration) {
    return clamp01((ms - start) / duration);
}

} // namespace

QPixmap valkyrieIcon(int size) {
    if (size <= 0) return {};
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const int inset = qMax(1, size / 12);
    drawValkyrieGlyph(painter, QRect(inset, inset, size - inset * 2, size - inset * 2));
    return pixmap;
}

SplashDialog::SplashDialog(QWidget* parent)
    : QDialog(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(480, 480);

    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        move(available.center() - rect().center());
    }

    // Animated splash logo (embedded Qt resource, with a local-file fallback).
    gif_ = new QMovie(this);
    const QStringList gifCandidates = {
        QStringLiteral(":/splash/n64musplashlogo.gif"),
        QDir::currentPath() + QStringLiteral("/n64musplashlogo.gif"),
    };
    for (const QString& candidate : gifCandidates) {
        gif_->setFileName(candidate);
        if (gif_->isValid()) {
            gif_->setCacheMode(QMovie::CacheAll);
            gif_->start();
            break;
        }
    }

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &SplashDialog::tick);
    elapsed_.start();
    timer_->start(16);
}

void SplashDialog::tick() {
    update();
    if (elapsed_.elapsed() >= DURATION_MS) {
        accept();
    }
}

void SplashDialog::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const qreal ms = elapsed_.elapsed();
    const qreal seconds = ms / 1000.0;
    const qreal t = clamp01(ms / DURATION_MS);

    // Whole-card fade in/out.
    qreal fade = 1.0;
    if (t < 0.10) fade = t / 0.10;
    else if (t > 0.88) fade = (1.0 - t) / 0.12;

    // Background vignette.
    QRadialGradient bgGradient(QPointF(width() / 2.0, height() / 3.0), width() / 1.6);
    bgGradient.setColorAt(0.0, QColor(27, 27, 40));
    bgGradient.setColorAt(1.0, QColor(10, 10, 16));
    painter.fillRect(rect(), bgGradient);

    const QPointF logoCenter(width() / 2.0, 185.0);
    const qreal pulse = 0.5 + 0.5 * std::sin(seconds * 2.2);

    // Breathing glow behind the logo.
    QRadialGradient glow(logoCenter, 175.0);
    glow.setColorAt(0.0, QColor(80, 150, 220, 26 + static_cast<int>(26 * pulse)));
    glow.setColorAt(1.0, QColor(80, 150, 220, 0));
    painter.fillRect(QRectF(logoCenter.x() - 180, logoCenter.y() - 180, 360.0, 360.0), glow);

    // Pulsing gold ring around the logo.
    painter.setPen(QPen(QColor(215, 175, 70, 55 + static_cast<int>(30 * pulse)), 2));
    const qreal ringRadius = 128.0 + 4.0 * std::sin(seconds * 2.0);
    painter.drawEllipse(logoCenter, ringRadius, ringRadius);

    // Logo: scale-in with overshoot, then gentle float.
    const qreal logoIn = easeOutBack(windowed(ms, 0.0, 550.0));
    if (logoIn > 0.001) {
        const qreal floatY = 4.0 * std::sin(seconds * 2.0);
        const qreal logoScale = logoIn * (1.0 + 0.02 * std::sin(seconds * 3.0));
        painter.save();
        painter.setOpacity(fade);
        painter.translate(logoCenter.x(), logoCenter.y() + floatY);
        painter.scale(logoScale, logoScale);
        if (gif_ && gif_->isValid()) {
            const QImage frame = gif_->currentImage();
            painter.drawImage(QRectF(-120.0, -120.0, 240.0, 240.0), frame, QRectF(frame.rect()));
        } else {
            drawValkyrieGlyph(painter, QRect(-96, -96, 192, 192));
        }
        painter.restore();
    }

    // "N64" letters pop in one by one with overshoot.
    {
        QFont logoFont = painter.font();
        logoFont.setPointSize(44);
        logoFont.setBold(true);
        const QFontMetricsF fm(logoFont);
        const QString logo = QStringLiteral("N64");
        const qreal spacing = 6.0;

        QVector<qreal> widths;
        qreal total = 0.0;
        for (QChar ch : logo) {
            const qreal charWidth = fm.horizontalAdvance(ch);
            widths.append(charWidth);
            total += charWidth;
        }
        total += spacing * (logo.length() - 1);

        const qreal baselineY = 375.0;
        qreal x = width() / 2.0 - total / 2.0;
        for (int i = 0; i < logo.length(); ++i) {
            const qreal p = easeOutBack(windowed(ms, 700.0 + i * 160.0, 430.0));
            if (p > 0.001 && fade > 0.0) {
                painter.save();
                painter.setOpacity(fade * clamp01(p));
                painter.translate(x + widths[i] / 2.0, baselineY);
                painter.scale(p, p);
                const QChar ch = logo.at(i);
                // Soft cyan glow pass, then silver-to-blue gradient fill.
                QPen glowPen(QColor(90, 175, 255, 110));
                glowPen.setWidth(5);
                glowPen.setBrush(QColor(90, 175, 255, 45));
                painter.setPen(glowPen);
                painter.drawText(QPointF(-widths[i] / 2.0, 0.0), QString(ch));
                QLinearGradient fill(0.0, -42.0, 0.0, 2.0);
                fill.setColorAt(0.0, QColor(240, 244, 252));
                fill.setColorAt(1.0, QColor(130, 185, 235));
                QPen fillPen;
                fillPen.setBrush(fill);
                fillPen.setWidth(0);
                painter.setPen(fillPen);
                painter.drawText(QPointF(-widths[i] / 2.0, 0.0), QString(ch));
                painter.restore();
            }
            x += widths[i] + spacing;
        }
    }

    // "FORGE STUDIO" slides up into place beneath the wordmark.
    const qreal titleIn = easeOutCubic(windowed(ms, 1300.0, 500.0));
    if (titleIn > 0.001 && fade > 0.0) {
        QFont titleFont;
        titleFont.setPointSize(15);
        titleFont.setBold(true);
        titleFont.setLetterSpacing(QFont::PercentageSpacing, 200);
        painter.setFont(titleFont);
        painter.setOpacity(fade * titleIn);
        QLinearGradient gold(QPointF(60.0, 0.0), QPointF(width() - 60.0, 0.0));
        gold.setColorAt(0.0, QColor(215, 175, 70));
        gold.setColorAt(0.5, QColor(245, 225, 170));
        gold.setColorAt(1.0, QColor(215, 175, 70));
        QPen goldPen;
        goldPen.setBrush(gold);
        painter.setPen(goldPen);
        const int slide = static_cast<int>((1.0 - titleIn) * 12.0);
        painter.drawText(QRect(0, 388 + slide, width(), 30), Qt::AlignCenter,
                         QStringLiteral("FORGE STUDIO"));
    }

    // Tagline.
    const qreal tagIn = easeOutCubic(windowed(ms, 1700.0, 450.0));
    if (tagIn > 0.001 && fade > 0.0) {
        QFont tagFont;
        tagFont.setPointSize(11);
        tagFont.setBold(false);
        painter.setFont(tagFont);
        painter.setOpacity(fade * tagIn);
        painter.setPen(QColor(160, 160, 180));
        const int slide = static_cast<int>((1.0 - tagIn) * 8.0);
        painter.drawText(QRect(0, 424 + slide, width(), 26), Qt::AlignCenter,
                         QStringLiteral("Emulator + Homebrew Toolkit"));
    }

    // Progress bar.
    const qreal bar = easeOutCubic(windowed(ms, 250.0, static_cast<qreal>(DURATION_MS) - 500.0));
    if (bar > 0.001 && fade > 0.0) {
        const qreal trackX = width() / 2.0 - 140.0;
        const qreal trackY = 452.0;
        painter.setPen(Qt::NoPen);
        painter.setOpacity(fade * 0.25);
        painter.setBrush(QColor(255, 255, 255));
        painter.drawRoundedRect(QRectF(trackX, trackY, 280.0, 4.0), 2.0, 2.0);
        if (bar > 0.0) {
            QLinearGradient barGrad(trackX, 0.0, trackX + 280.0, 0.0);
            barGrad.setColorAt(0.0, QColor(90, 170, 255));
            barGrad.setColorAt(1.0, QColor(215, 175, 70));
            painter.setOpacity(fade);
            painter.setBrush(barGrad);
            painter.drawRoundedRect(QRectF(trackX, trackY, 280.0 * bar, 4.0), 2.0, 2.0);
        }
    }
}

void SplashDialog::mousePressEvent(QMouseEvent* /*event*/) {
    accept();
}

} // namespace n64forge::studio