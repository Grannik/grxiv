#include <QApplication>
#include <QMainWindow>
#include <QImage>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QWheelEvent>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QKeyEvent>
#include <QDir>
#include <QFileInfoList>
#include <QBuffer>
#include <QImageReader>
#include <QFile>

class ImageGLWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    ImageGLWidget(const QString& path, QWidget* parent = nullptr)
        : QOpenGLWidget(parent), shaderProgram(nullptr), texture(nullptr), zoomLevel(1.0f), currentImageIndex(-1) {
        QSurfaceFormat format;
        format.setVersion(2, 1);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        setFormat(format);
        setFocusPolicy(Qt::StrongFocus);

        QDir dir(path);
        if (dir.exists()) {
            QStringList filters;
            filters << "*.jpg" << "*.jpeg" << "*.png" << "*.bmp" << "*.gif";
            dir.setNameFilters(filters);
            dir.setFilter(QDir::Files | QDir::NoDotAndDotDot);
            dir.setSorting(QDir::Name);
            imageFiles = dir.entryInfoList();
            if (imageFiles.isEmpty()) {
                QApplication::quit();
                return;
            }
        } else {
            QFileInfo fileInfo(path);
            if (fileInfo.isFile()) {
                imageFiles << fileInfo;
            } else {
                QApplication::quit();
                return;
            }
        }
    }

    ImageGLWidget(const QImage& clipboardImage, QWidget* parent = nullptr)
        : QOpenGLWidget(parent), shaderProgram(nullptr), texture(nullptr), zoomLevel(1.0f), currentImageIndex(0) {
        QSurfaceFormat format;
        format.setVersion(2, 1);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        setFormat(format);
        setFocusPolicy(Qt::StrongFocus);

        image = clipboardImage;
        if (image.isNull()) {
            QApplication::quit();
            return;
        }
        imageFiles << QFileInfo("clipboard_image");
    }

    ~ImageGLWidget() {
        makeCurrent();
        delete texture;
        delete shaderProgram;
        glDeleteBuffers(1, &VBO);
        glDeleteBuffers(1, &EBO);
        doneCurrent();
    }

protected:
    void initializeGL() override {
        if (!context()) {
            return;
        }

        initializeOpenGLFunctions();

        shaderProgram = new QOpenGLShaderProgram(this);
        if (!shaderProgram) {
            return;
        }

        bool success = true;
        success &= shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
            "#version 120\n"
            "attribute vec2 position;\n"
            "attribute vec2 texCoord;\n"
            "varying vec2 vTexCoord;\n"
            "uniform mat4 mvp;\n"
            "void main() {\n"
            "    gl_Position = mvp * vec4(position, 0.0, 1.0);\n"
            "    vTexCoord = texCoord;\n"
            "}\n");
        success &= shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#version 120\n"
            "varying vec2 vTexCoord;\n"
            "uniform sampler2D tex;\n"
            "void main() {\n"
            "    gl_FragColor = texture2D(tex, vTexCoord);\n"
            "}\n");
        success &= shaderProgram->link();
        if (!success) {
            return;
        }

        float vertices[] = {
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 0.0f
        };
        unsigned int indices[] = {
            0, 1, 2,
            2, 3, 0
        };

        glGenBuffers(1, &VBO);
        if (VBO == 0) {
            return;
        }
        glGenBuffers(1, &EBO);
        if (EBO == 0) {
            return;
        }

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        if (!imageFiles.isEmpty()) {
            if (image.isNull()) {
                loadImage(0);
            } else {
                image = image.convertToFormat(QImage::Format_ARGB32);
                updateTexture();
            }
        }
    }

    void paintGL() override {
        glClear(GL_COLOR_BUFFER_BIT);

        if (!texture || !texture->isCreated() || !shaderProgram || !shaderProgram->isLinked()) {
            return;
        }

        shaderProgram->bind();

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);

        GLint posLoc = shaderProgram->attributeLocation("position");
        GLint texLoc = shaderProgram->attributeLocation("texCoord");
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(posLoc);
        glEnableVertexAttribArray(texLoc);

        QMatrix4x4 mvp;
        float imageAspect = static_cast<float>(image.width()) / image.height();
        float windowAspect = static_cast<float>(width()) / height();
        float scaleX = 1.0f;
        float scaleY = 1.0f;

        if (imageAspect > windowAspect) {
            scaleY = windowAspect / imageAspect;
        } else {
            scaleX = imageAspect / windowAspect;
        }

        mvp.scale(scaleX * zoomLevel, scaleY * zoomLevel, 1.0f);
        shaderProgram->setUniformValue("mvp", mvp);

        texture->bind();
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        glDisableVertexAttribArray(posLoc);
        glDisableVertexAttribArray(texLoc);
        shaderProgram->release();
    }

    void resizeGL(int w, int h) override {
        glViewport(0, 0, w, h);
    }

    void wheelEvent(QWheelEvent* event) override {
        float delta = event->angleDelta().y() > 0 ? 1.1f : 0.9f;
        zoomLevel *= delta;
        zoomLevel = qMax(0.1f, qMin(zoomLevel, 10.0f));
        update();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Q) {
            window()->close();
        } else if (event->key() == Qt::Key_Left) {
            loadPreviousImage();
        } else if (event->key() == Qt::Key_Right) {
            loadNextImage();
        }
        QOpenGLWidget::keyPressEvent(event);
    }

private:
    void loadImage(int index) {
        if (index < 0 || index >= imageFiles.size()) {
            return;
        }
        currentImageIndex = index;
        window()->setWindowTitle(QString("%1 (%2/%3)").arg(imageFiles[index].fileName()).arg(index + 1).arg(imageFiles.size()));
        QString imagePath = imageFiles[index].absoluteFilePath();
        if (!image.load(imagePath)) {
            QApplication::quit();
            return;
        }
        if (image.isNull()) {
            QApplication::quit();
            return;
        }
        image = image.convertToFormat(QImage::Format_ARGB32);
        zoomLevel = 1.0f;
        updateTexture();
        update();
    }

    void loadNextImage() {
        if (currentImageIndex + 1 < imageFiles.size()) {
            loadImage(currentImageIndex + 1);
        }
    }

    void loadPreviousImage() {
        if (currentImageIndex - 1 >= 0) {
            loadImage(currentImageIndex - 1);
        }
    }

    void updateTexture() {
        makeCurrent();
        if (texture) {
            texture->destroy();
            delete texture;
            texture = nullptr;
        }
        if (!image.isNull()) {
            texture = new QOpenGLTexture(image);
            if (texture && texture->isCreated()) {
                texture->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
                texture->setMagnificationFilter(QOpenGLTexture::Linear);
                texture->generateMipMaps();
            } else {
                delete texture;
                texture = nullptr;
            }
        }
        doneCurrent();
    }

    QImage image;
    QOpenGLShaderProgram* shaderProgram;
    QOpenGLTexture* texture;
    unsigned int VBO, EBO;
    float zoomLevel;
    QFileInfoList imageFiles;
    int currentImageIndex;
};

class ImageViewer : public QMainWindow {
    Q_OBJECT
public:
    ImageViewer(const QString& path, QWidget* parent = nullptr)
        : QMainWindow(parent) {
        setWindowTitle(QFileInfo(path).fileName());
        resize(800, 600);

        ImageGLWidget* glWidget = new ImageGLWidget(path, this);
        setCentralWidget(glWidget);
    }

    ImageViewer(const QImage& clipboardImage, QWidget* parent = nullptr)
        : QMainWindow(parent) {
        setWindowTitle("grxiv");
        resize(800, 600);

        ImageGLWidget* glWidget = new ImageGLWidget(clipboardImage, this);
        setCentralWidget(glWidget);
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument("path", "Path to image file or directory");
    parser.process(app);

    QStringList args = parser.positionalArguments();

    if (args.isEmpty()) {
        QByteArray stdinData;
        QFile stdinFile("/dev/stdin");
        if (stdinFile.open(QIODevice::ReadOnly)) {
            stdinData = stdinFile.readAll();
            stdinFile.close();
        }

        if (!stdinData.isEmpty()) {
            QImage clipboardImage;
            QBuffer buffer(&stdinData);
            buffer.open(QIODevice::ReadOnly);

            QImageReader reader(&buffer);
            if (!reader.canRead()) {
                return 1;
            }
            clipboardImage = reader.read();
            buffer.close();

            if (clipboardImage.isNull()) {
                return 1;
            }

            ImageViewer viewer(clipboardImage);
            viewer.show();
            return app.exec();
        }

        return 1;
    }

    ImageViewer viewer(args[0]);
    viewer.show();
    return app.exec();
}

#include "grxiv.moc"
