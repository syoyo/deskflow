/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "platform/XclipClipboardBridge.h"
#include "platform/EiClipboard.h"
#include "base/Log.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QImage>
#include <QImageReader>
#include <QProcess>
#include <QtEndian>
#include <algorithm>
#include <utility>

namespace deskflow {
namespace {
constexpr int timeoutMs = 750;
constexpr qint64 hardLimitBytes = 64 * 1024 * 1024;

void configure(QProcess &process, const QString &executable)
{
  process.setProgram(executable);
  // The xclip selection owner forks. Never let it retain the server socket,
  // portal connection, or other inherited descriptors (Deskflow issue #9386).
  process.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
  process.setStandardErrorFile(QProcess::nullDevice());
}

bool collect(QProcess &process, QByteArray &data, qint64 limit)
{
  QElapsedTimer timer;
  timer.start();
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(20);
    data += process.readAllStandardOutput();
    if (data.size() > limit || timer.elapsed() >= timeoutMs) {
      process.kill();
      process.waitForFinished(100);
      data.clear();
      return false;
    }
  }
  data += process.readAllStandardOutput();
  return data.size() <= limit && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QByteArray pngToDib(const QByteArray &png, qint64 limit)
{
  QBuffer input;
  input.setData(png);
  input.open(QIODevice::ReadOnly);
  QImageReader reader(&input, "PNG");
  const auto size = reader.size();
  if (!size.isValid() || qint64(size.width()) * size.height() > limit / 4)
    return {};
  const auto image = reader.read();
  if (image.isNull())
    return {};
  QByteArray bmp;
  QBuffer buffer(&bmp);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "BMP") || bmp.size() < 14 || bmp.size() - 14 > limit)
    return {};
  return bmp.mid(14);
}

QByteArray dibToPng(const QByteArray &dib, qint64 limit)
{
  // Deskflow uses a Windows DIB (without the 14-byte BMP file header).
  if (dib.size() < 40)
    return {};
  const auto *bytes = reinterpret_cast<const uchar *>(dib.constData());
  const quint32 headerSize = qFromLittleEndian<quint32>(bytes);
  const qint64 width = qFromLittleEndian<qint32>(bytes + 4);
  const qint64 height = qFromLittleEndian<qint32>(bytes + 8);
  const quint16 depth = qFromLittleEndian<quint16>(bytes + 14);
  const quint32 compression = qFromLittleEndian<quint32>(bytes + 16);
  const quint32 colors = qFromLittleEndian<quint32>(bytes + 32);
  if (headerSize < 40 || headerSize > quint32(dib.size()) || depth == 0 || depth > 32)
    return {};
  if (width <= 0 || height == 0 || width * qAbs(height) > limit / 4)
    return {};
  const quint64 paletteSize = quint64(colors ? colors : (depth <= 8 ? 1u << depth : 0)) * 4;
  const quint64 masks = headerSize == 40 ? (compression == 3 ? 12 : compression == 6 ? 16 : 0) : 0;
  const quint64 offset = 14 + headerSize + paletteSize + masks;
  if (offset > quint64(dib.size()) + 14)
    return {};
  QByteArray bmp;
  QDataStream stream(&bmp, QIODevice::WriteOnly);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.writeRawData("BM", 2);
  stream << quint32(dib.size() + 14) << quint32(0) << quint32(offset);
  stream.writeRawData(dib.constData(), int(dib.size()));
  QImage image;
  if (!image.loadFromData(bmp, "BMP"))
    return {};
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "PNG"))
    return {};
  return png;
}
} // namespace

XclipClipboardBridge::XclipClipboardBridge(QString executable) : m_executable(std::move(executable))
{
}

bool XclipClipboardBridge::copyTo(IClipboard *destination, EiClipboard *cache)
{
  std::scoped_lock lock(m_mutex);
  return IClipboard::copy(destination, cache);
}

QByteArray XclipClipboardBridge::fingerprint(IClipboard::Format format, const QByteArray &data)
{
  return QByteArray::number(int(format)) + ':' + QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

bool XclipClipboardBridge::read(const QString &target, QByteArray &data, qint64 maxBytes) const
{
  QProcess process;
  configure(process, m_executable);
  process.setArguments({"-selection", "clipboard", "-out", "-target", target});
  process.start();
  process.closeWriteChannel();
  if (!process.waitForStarted(100))
    return false;
  return collect(process, data, maxBytes);
}

bool XclipClipboardBridge::write(const QString &target, const QByteArray &data) const
{
  QProcess process;
  configure(process, m_executable);
  // Default silent mode forks a selection owner and exits after taking ownership.
  // Keep stdin binary; no shell, command-line clipboard contents, or temp files.
  process.setArguments({"-selection", "clipboard", "-in", "-target", target});
  process.start();
  if (!process.waitForStarted(100))
    return false;
  process.write(data);
  process.closeWriteChannel();
  QByteArray ignored;
  return collect(process, ignored, 4096);
}

bool XclipClipboardBridge::refresh(EiClipboard *cache, qint64 maxBytes)
{
  std::scoped_lock lock(m_mutex);
  maxBytes = std::min(maxBytes, hardLimitBytes);
  if (maxBytes <= 0)
    return false;
  QByteArray targets;
  if (!read("TARGETS", targets, 64 * 1024))
    return false; // Failed/unsupported reads must not erase a valid cache.
  const auto offered = targets.split('\n');
  const char *const textTargets[] = {"UTF8_STRING", "text/plain;charset=utf-8", "text/plain"};
  QString target;
  IClipboard::Format format = IClipboard::Format::Text;
  if (offered.contains("image/png")) {
    target = "image/png";
    format = IClipboard::Format::Bitmap;
  } else {
    for (const auto *textTarget : textTargets) {
      if (offered.contains(textTarget)) {
        target = QString::fromLatin1(textTarget);
        break;
      }
    }
  }
  if (target.isEmpty())
    return false;
  QByteArray data;
  if (!read(target, data, maxBytes))
    return false;
  if (format == IClipboard::Format::Bitmap) {
    data = pngToDib(data, maxBytes);
    if (data.isEmpty())
      return false;
  }
  const auto hash = fingerprint(format, data);
  if (hash == m_lastFingerprint)
    return false;
  cache->open(0);
  cache->empty();
  cache->add(format, data.toStdString());
  cache->close();
  m_lastFingerprint = hash;
  LOG_DEBUG("xclip clipboard changed (%lld bytes)", static_cast<long long>(data.size()));
  return true;
}

bool XclipClipboardBridge::publish(const IClipboard *source, EiClipboard *cache, qint64 maxBytes)
{
  std::scoped_lock lock(m_mutex);
  maxBytes = std::min(maxBytes, hardLimitBytes);
  if (maxBytes <= 0 || !source->open(0))
    return false;
  const auto format = source->has(IClipboard::Format::Bitmap) ? IClipboard::Format::Bitmap : IClipboard::Format::Text;
  const bool supported = source->has(format);
  const auto data = supported ? QByteArray::fromStdString(source->get(format)) : QByteArray();
  source->close();
  if (!supported || data.size() > maxBytes)
    return false;
  const auto encoded = format == IClipboard::Format::Bitmap ? dibToPng(data, maxBytes) : data;
  if ((format == IClipboard::Format::Bitmap && encoded.isEmpty()) || encoded.size() > maxBytes)
    return false;
  if (!write(format == IClipboard::Format::Bitmap ? "image/png" : "UTF8_STRING", encoded)) {
    LOG_WARN("xclip clipboard write failed or timed out");
    return false;
  }
  // The next read must not bounce the received clipboard back to its sender.
  cache->open(0);
  cache->empty();
  cache->add(format, data.toStdString());
  cache->close();
  m_lastFingerprint = fingerprint(format, data);
  return true;
}
} // namespace deskflow
