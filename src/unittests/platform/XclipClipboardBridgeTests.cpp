/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "arch/Arch.h"
#include "base/Log.h"
#include "deskflow/Clipboard.h"
#include "platform/EiClipboard.h"
#include "platform/XclipClipboardBridge.h"
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QScopeGuard>
#include <fcntl.h>
#include <unistd.h>

using namespace deskflow;

class XclipClipboardBridgeTests : public QObject
{
  Q_OBJECT
private:
  Arch m_arch;
  Log m_log;
  QTemporaryDir m_dir;

  void put(const QString &name, const QByteArray &data)
  {
    QFile file(m_dir.filePath(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
  }
  QString executable() const { return m_dir.filePath("xclip"); }

private Q_SLOTS:
  void initTestCase()
  {
    QVERIFY(m_dir.isValid());
    put("xclip", R"PY(#!/usr/bin/python3
import os,pathlib,sys,time
root=pathlib.Path(__file__).parent
if (root/'checkfd').exists():
    try:
        os.fstat(int((root/'checkfd').read_text()))
        (root/'inherited').write_text('leaked')
    except OSError: pass
args=sys.argv[1:]
target=args[args.index('-target')+1]
if (root/'fail').exists(): sys.exit(1)
if (root/'hang').exists(): time.sleep(4)
if '-in' in args:
    (root/'data').write_bytes(sys.stdin.buffer.read())
    (root/'targets').write_text(target+'\n')
else:
    sys.stdout.buffer.write((root/('targets' if target=='TARGETS' else 'data')).read_bytes())
)PY");
    QVERIFY(QFile::setPermissions(executable(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  }
  void init()
  {
    QFile::remove(m_dir.filePath("fail"));
    QFile::remove(m_dir.filePath("hang"));
    QFile::remove(m_dir.filePath("checkfd"));
    QFile::remove(m_dir.filePath("inherited"));
    put("targets", "UTF8_STRING\ntext/plain\n");
    put("data", "initial");
  }
  void detectsContentChangesWithUnchangedTargets()
  {
    XclipClipboardBridge bridge(executable());
    EiClipboard cache(kClipboardClipboard);
    QVERIFY(bridge.refresh(&cache, 1024));
    QVERIFY(!bridge.refresh(&cache, 1024));
    put("data", "different UTF-8: \xe6\x97\xa5\xe6\x9c\xac");
    QVERIFY(bridge.refresh(&cache, 1024));
    cache.open(0);
    QCOMPARE(cache.get(IClipboard::Format::Text), std::string("different UTF-8: \xe6\x97\xa5\xe6\x9c\xac"));
    cache.close();
  }
  void preservesCacheOnFailureAndOversize()
  {
    XclipClipboardBridge bridge(executable());
    EiClipboard cache(kClipboardClipboard);
    QVERIFY(bridge.refresh(&cache, 1024));
    put("data", QByteArray(2048, 'a'));
    QVERIFY(!bridge.refresh(&cache, 1024));
    put("fail", "1");
    QVERIFY(!bridge.refresh(&cache, 1024));
    cache.open(0);
    QCOMPARE(cache.get(IClipboard::Format::Text), std::string("initial"));
    cache.close();
  }
  void publishesBinarySafeTextWithoutEcho()
  {
    XclipClipboardBridge bridge(executable());
    EiClipboard cache(kClipboardClipboard);
    Clipboard source;
    source.open(0);
    source.empty();
    const std::string contents("a\0b\n'\"$()", 10);
    source.add(IClipboard::Format::Text, contents);
    source.close();
    QVERIFY(bridge.publish(&source, &cache, 1024));
    QFile data(m_dir.filePath("data"));
    QVERIFY(data.open(QIODevice::ReadOnly));
    QCOMPARE(data.readAll(), QByteArray::fromStdString(contents));
    QVERIFY(!bridge.refresh(&cache, 1024));
  }
  void roundTripsPngAndDib()
  {
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    put("targets", "image/png\n");
    put("data", png);
    XclipClipboardBridge bridge(executable());
    EiClipboard cache(kClipboardClipboard);
    QVERIFY(bridge.refresh(&cache, 4096));
    Clipboard source;
    QVERIFY(bridge.copyTo(&source, &cache));
    QVERIFY(bridge.publish(&source, &cache, 4096));
    QFile file(m_dir.filePath("data"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QImage result;
    QVERIFY(result.loadFromData(file.readAll(), "PNG"));
    QCOMPARE(result.pixelColor(0, 0), QColor(Qt::red));
  }
  void timesOutUnresponsiveHelper()
  {
    put("hang", "1");
    XclipClipboardBridge bridge(executable());
    EiClipboard cache(kClipboardClipboard);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!bridge.refresh(&cache, 1024));
    QVERIFY(timer.elapsed() < 2000);
  }
  void closesInheritedDescriptors()
  {
    const int fd = ::open("/dev/null", O_RDONLY);
    QVERIFY(fd >= 0);
    auto cleanupFd = qScopeGuard([&] { ::close(fd); });
    const int inheritedFd = fcntl(fd, F_DUPFD, 150);
    QVERIFY(inheritedFd >= 0);
    auto cleanupInherited = qScopeGuard([&] { ::close(inheritedFd); });
    put("checkfd", QByteArray::number(inheritedFd));
    XclipClipboardBridge bridge(executable());
    EiClipboard cache(kClipboardClipboard);
    QVERIFY(bridge.refresh(&cache, 1024));
    QVERIFY(!QFile::exists(m_dir.filePath("inherited")));
  }
  void realXclipRoundTripOnIsolatedXServer()
  {
    const auto xclip = QStandardPaths::findExecutable("xclip");
    const auto xvfb = QStandardPaths::findExecutable("Xvfb");
    if (xclip.isEmpty() || xvfb.isEmpty())
      QSKIP("xclip and Xvfb required for isolated integration test");
    QProcess server;
    server.setProgram(xvfb);
    server.setArguments({"-displayfd", "1", "-screen", "0", "800x600x24", "-nolisten", "tcp"});
    server.start();
    QVERIFY(server.waitForStarted(1000));
    auto cleanupServer = qScopeGuard([&] { server.kill(); server.waitForFinished(1000); });
    QVERIFY(server.waitForReadyRead(3000));
    const auto display = server.readLine().trimmed();
    bool valid = false;
    display.toInt(&valid);
    QVERIFY(valid);
    const auto previousDisplay = qgetenv("DISPLAY");
    const bool hadDisplay = qEnvironmentVariableIsSet("DISPLAY");
    auto cleanupDisplay = qScopeGuard([&] {
      if (hadDisplay) qputenv("DISPLAY", previousDisplay); else qunsetenv("DISPLAY");
    });
    qputenv("DISPLAY", ':' + display);
    Clipboard source;
    source.open(0);
    source.empty();
    source.add(IClipboard::Format::Text, "Deskflow xclip integration \xe6\x97\xa5\xe6\x9c\xac\n");
    source.close();
    EiClipboard sent(kClipboardClipboard), received(kClipboardClipboard);
    XclipClipboardBridge writer(xclip), reader(xclip);
    QVERIFY(writer.publish(&source, &sent, 4096));
    QVERIFY(reader.refresh(&received, 4096));
    received.open(0);
    QCOMPARE(received.get(IClipboard::Format::Text), std::string("Deskflow xclip integration \xe6\x97\xa5\xe6\x9c\xac\n"));
    received.close();
    // Same targets, new content: this was missed by the old fork's monitor.
    source.open(0);
    source.empty();
    source.add(IClipboard::Format::Text, "second copy");
    source.close();
    QVERIFY(writer.publish(&source, &sent, 4096));
    QVERIFY(reader.refresh(&received, 4096));
    const std::string large(256 * 1024, 'x');
    source.open(0);
    source.empty();
    source.add(IClipboard::Format::Text, large);
    source.close();
    QVERIFY(writer.publish(&source, &sent, 512 * 1024));
    QVERIFY(reader.refresh(&received, 512 * 1024));
    received.open(0);
    QCOMPARE(received.get(IClipboard::Format::Text), large);
    received.close();
  }
};

QTEST_GUILESS_MAIN(XclipClipboardBridgeTests)
#include "XclipClipboardBridgeTests.moc"
