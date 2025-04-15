#include <QApplication>
#include <QMainWindow>
#include <QImage>
#include <QMessageBox>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QWheelEvent>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QDebug>
#include <QKeyEvent>
#include <QDir>
#include <QFileInfoList>

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

        // Check if the path is a directory or a file
        QDir dir(path);
        if (dir.exists()) {
            // Load the list of images from the directory
            QStringList filters;
            filters << "*.jpg" << "*.jpeg" << "*.png" << "*.bmp" << "*.gif";
            dir.setNameFilters(filters);
            dir.setFilter(QDir::Files | QDir::NoDotAndDotDot);
            dir.setSorting(QDir::Name);
            imageFiles = dir.entryInfoList();
            if (imageFiles.isEmpty()) {
                qDebug() << "No images in directory:" << path;
                QMessageBox::critical(this, "Error", "No images in directory: " + path);
                return;
            }
        } else {
            // Check if the path is a file
            QFileInfo fileInfo(path);
            if (fileInfo.isFile()) {
                imageFiles << fileInfo;
            } else {
                qDebug() << "Invalid path:" << path;
                QMessageBox::critical(this, "Error", "Invalid path specified: " + path);
                return;
            }
        }
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
            qDebug() << "OpenGL context not created!";
            return;
        }

        initializeOpenGLFunctions();

        const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* glslVersion = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        qDebug() << "OpenGL Version:" << (glVersion ? glVersion : "unknown");
        qDebug() << "GLSL Version:" << (glslVersion ? glslVersion : "unknown");

        shaderProgram = new QOpenGLShaderProgram(this);
        if (!shaderProgram) {
            qDebug() << "Error creating shader program object";
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
        if (!success) {
            qDebug() << "Vertex shader compilation error:" << shaderProgram->log();
            return;
        }
        success &= shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
            "#version 120\n"
            "varying vec2 vTexCoord;\n"
            "uniform sampler2D tex;\n"
            "void main() {\n"
            "    gl_FragColor = texture2D(tex, vTexCoord);\n"
            "}\n");
        if (!success) {
            qDebug() << "Fragment shader compilation error:" << shaderProgram->log();
            return;
        }
        success &= shaderProgram->link();
        if (!success) {
            qDebug() << "Shader linking error:" << shaderProgram->log();
            return;
        }
        qDebug() << "Shaders successfully initialized";

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
            qDebug() << "Error creating VBO";
            return;
        }
        glGenBuffers(1, &EBO);
        if (EBO == 0) {
            qDebug() << "Error creating EBO";
            return;
        }

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        // Load the first image after OpenGL initialization
        if (!imageFiles.isEmpty()) {
            loadImage(0);
        }
    }

    void paintGL() override {
        glClear(GL_COLOR_BUFFER_BIT);

        if (!texture || !texture->isCreated() || !shaderProgram || !shaderProgram->isLinked()) {
            qDebug() << "Cannot render: texture or shader error";
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
        QString imagePath = imageFiles[index].absoluteFilePath();
        qDebug() << "Loading image:" << imagePath;
        if (!image.load(imagePath)) {
            qDebug() << "Failed to load image:" << imagePath;
            QMessageBox::critical(this, "Error", "Failed to load image: " + imagePath);
            return;
        }
        if (image.isNull()) {
            qDebug() << "The image is empty after loading";
            return;
        }
        image = image.convertToFormat(QImage::Format_ARGB32);
        qDebug() << "The image is successfully loaded. Size:" << image.size();
        zoomLevel = 1.0f; // Reset zoom when loading a new image
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
        qDebug() << "Context active:" << (context() && context()->isValid());
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
                qDebug() << "Texture updated";
            } else {
                qDebug() << "Texture creation error";
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
        setWindowTitle(" grxiv");
        resize(800, 600);

        ImageGLWidget* glWidget = new ImageGLWidget(path, this);
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
        QMessageBox::critical(nullptr, "Error", "No path specified.\nUsage: grxiv <path_to_image_or_directory>");
        return 1;
    }

    ImageViewer viewer(args[0]);
    viewer.show();
    return app.exec();
}

#include "grxiv.moc"
