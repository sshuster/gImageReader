/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * HOCRPdfExporter.cc
 * Copyright (C) 2013-2017 Sandro Mani <manisandro@gmail.com>
 *
 * gImageReader is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gImageReader is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstring>
#include <QBuffer>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFontDialog>
#include <QGraphicsPixmapItem>
#include <QMessageBox>
#include <QPainter>
#include <QStandardItemModel>
#include <podofo/base/PdfDictionary.h>
#include <podofo/base/PdfFilter.h>
#include <podofo/base/PdfStream.h>
#include <podofo/doc/PdfFont.h>
#include <podofo/doc/PdfIdentityEncoding.h>
#include <podofo/doc/PdfImage.h>
#include <podofo/doc/PdfPage.h>
#include <podofo/doc/PdfPainter.h>
#include <podofo/doc/PdfStreamedDocument.h>
#define USE_STD_NAMESPACE
#include <tesseract/baseapi.h>
#undef USE_STD_NAMESPACE

#include "CCITTFax4Encoder.hh"
#include "Config.hh"
#include "Displayer.hh"
#include "DisplayerToolHOCR.hh"
#include "FileDialogs.hh"
#include "HOCRDocument.hh"
#include "HOCRPdfExporter.hh"
#include "MainWindow.hh"
#include "SourceManager.hh"

class HOCRPdfExporter::QPainterPDFPainter : public HOCRPdfExporter::PDFPainter {
public:
	QPainterPDFPainter(QPainter* painter) : m_painter(painter) {
		m_curFontSize = m_painter->font().pointSize();
	}
	void setFontSize(double pointSize) override {
		if(pointSize != m_curFontSize) {
			QFont font = m_painter->font();
			font.setPointSize(pointSize);
			m_painter->setFont(font);
			m_curFontSize = pointSize;
		}
	}
	void drawText(double x, double y, const QString& text) override {
		m_painter->drawText(x, y, text);
	}
	void drawImage(const QRect& bbox, const QImage& image, const PDFSettings& settings) override {
		QImage img = convertedImage(image, settings.colorFormat, settings.conversionFlags);
		if(settings.compression == PDFSettings::CompressJpeg) {
			QByteArray data;
			QBuffer buffer(&data);
			img.save(&buffer, "jpg", settings.compressionQuality);
			img = QImage::fromData(data);
		}
		m_painter->drawImage(bbox, img);
	}
	double getAverageCharWidth() const override {
		return m_painter->fontMetrics().averageCharWidth();
	}
	double getTextWidth(const QString& text) const override {
		return m_painter->fontMetrics().width(text);
	}

private:
	QPainter* m_painter;
	int m_curFontSize;
};

#if PODOFO_VERSION < PODOFO_MAKE_VERSION(0,9,3)
namespace PoDoFo {
class PdfImageCompat : public PoDoFo::PdfImage {
	using PdfImage::PdfImage;
public:
	void SetImageDataRaw( unsigned int nWidth, unsigned int nHeight,
						  unsigned int nBitsPerComponent, PdfInputStream* pStream ) {
		m_rRect.SetWidth( nWidth );
		m_rRect.SetHeight( nHeight );

		this->GetObject()->GetDictionary().AddKey( "Width",  PdfVariant( static_cast<pdf_int64>(nWidth) ) );
		this->GetObject()->GetDictionary().AddKey( "Height", PdfVariant( static_cast<pdf_int64>(nHeight) ) );
		this->GetObject()->GetDictionary().AddKey( "BitsPerComponent", PdfVariant( static_cast<pdf_int64>(nBitsPerComponent) ) );

		PdfVariant var;
		m_rRect.ToVariant( var );
		this->GetObject()->GetDictionary().AddKey( "BBox", var );

		this->GetObject()->GetStream()->SetRawData( pStream, -1 );
	}
};
}
#endif

class HOCRPdfExporter::PoDoFoPDFPainter : public HOCRPdfExporter::PDFPainter {
public:
	PoDoFoPDFPainter(PoDoFo::PdfDocument* document, PoDoFo::PdfPainter* painter, double scaleFactor)
		: m_document(document), m_painter(painter), m_scaleFactor(scaleFactor) {
		m_pageHeight = m_painter->GetPage()->GetPageSize().GetHeight();
	}
	void setFontSize(double pointSize) override {
		m_painter->GetFont()->SetFontSize(pointSize);
	}
	void drawText(double x, double y, const QString& text) override {
		PoDoFo::PdfString pdfString(reinterpret_cast<const PoDoFo::pdf_utf8*>(text.toUtf8().data()));
		m_painter->DrawText(x * m_scaleFactor, m_pageHeight - y * m_scaleFactor, pdfString);
	}
	void drawImage(const QRect& bbox, const QImage& image, const PDFSettings& settings) override {
		QImage img = convertedImage(image, settings.colorFormat, settings.conversionFlags);
		if(settings.colorFormat == QImage::Format_Mono) {
			img.invertPixels();
		}
#if PODOFO_VERSION >= PODOFO_MAKE_VERSION(0,9,3)
		PoDoFo::PdfImage pdfImage(m_document);
#else
		PoDoFo::PdfImageCompat pdfImage(m_document);
#endif
		pdfImage.SetImageColorSpace(img.format() == QImage::Format_RGB888 ? PoDoFo::ePdfColorSpace_DeviceRGB : PoDoFo::ePdfColorSpace_DeviceGray);
		int width = img.width();
		int height = img.height();
		int sampleSize = settings.colorFormat == QImage::Format_Mono ? 1 : 8;
		if(settings.compression == PDFSettings::CompressZip) {
			// QImage has 32-bit aligned scanLines, but we need a continuous buffer
			int numComponents = settings.colorFormat == QImage::Format_RGB888 ? 3 : 1;
			int bytesPerLine = numComponents * ((width * sampleSize) / 8 + ((width * sampleSize) % 8 != 0));
			QVector<char> buf(bytesPerLine * height);
			for(int y = 0; y < height; ++y) {
				std::memcpy(buf.data() + y * bytesPerLine, img.scanLine(y), bytesPerLine);
			}
			PoDoFo::PdfMemoryInputStream is(buf.data(), bytesPerLine * height);
			pdfImage.SetImageData(width, height, sampleSize, &is, {PoDoFo::ePdfFilter_FlateDecode});
		} else if(settings.compression == PDFSettings::CompressJpeg) {
			PoDoFo::PdfName dctFilterName(PoDoFo::PdfFilterFactory::FilterTypeToName(PoDoFo::ePdfFilter_DCTDecode));
			pdfImage.GetObject()->GetDictionary().AddKey(PoDoFo::PdfName::KeyFilter, dctFilterName);
			QByteArray data;
			QBuffer buffer(&data);
			img.save(&buffer, "jpg", settings.compressionQuality);
			PoDoFo::PdfMemoryInputStream is(data.data(), data.size());
			pdfImage.SetImageDataRaw(width, height, sampleSize, &is);
		} else if(settings.compression == PDFSettings::CompressFax4) {
			PoDoFo::PdfName faxFilterName(PoDoFo::PdfFilterFactory::FilterTypeToName(PoDoFo::ePdfFilter_CCITTFaxDecode));
			pdfImage.GetObject()->GetDictionary().AddKey(PoDoFo::PdfName::KeyFilter, faxFilterName);
			PoDoFo::PdfDictionary decodeParams;
			decodeParams.AddKey("Columns", PoDoFo::PdfObject(PoDoFo::pdf_int64(img.width())));
			decodeParams.AddKey("Rows", PoDoFo::PdfObject(PoDoFo::pdf_int64(img.height())));
			decodeParams.AddKey("K", PoDoFo::PdfObject(PoDoFo::pdf_int64(-1))); // K < 0 --- Pure two-dimensional encoding (Group 4)
			pdfImage.GetObject()->GetDictionary().AddKey("DecodeParms", PoDoFo::PdfObject(decodeParams));
			CCITTFax4Encoder encoder;
			uint32_t encodedLen = 0;
			uint8_t* encoded = encoder.encode(img.constBits(), img.width(), img.height(), img.bytesPerLine(), encodedLen);
			PoDoFo::PdfMemoryInputStream is(reinterpret_cast<char*>(encoded), encodedLen);
			pdfImage.SetImageDataRaw(img.width(), img.height(), sampleSize, &is);
		}
		m_painter->DrawImage(bbox.x() * m_scaleFactor, m_pageHeight - (bbox.y() + bbox.height()) * m_scaleFactor, &pdfImage, m_scaleFactor * bbox.width() / double(image.width()), m_scaleFactor * bbox.height() / double(image.height()));
	}
	double getAverageCharWidth() const override {
		return m_painter->GetFont()->GetFontMetrics()->CharWidth(static_cast<unsigned char>('x')) / m_scaleFactor;
	}
	double getTextWidth(const QString& text) const override {
		PoDoFo::PdfString pdfString(reinterpret_cast<const PoDoFo::pdf_utf8*>(text.toUtf8().data()));
		return m_painter->GetFont()->GetFontMetrics()->StringWidth(pdfString) / m_scaleFactor;
	}

private:
	PoDoFo::PdfDocument* m_document;
	PoDoFo::PdfPainter* m_painter;
	double m_scaleFactor;
	double m_pageHeight;
};


HOCRPdfExporter::HOCRPdfExporter(const HOCRDocument* hocrdocument, const HOCRPage* previewPage, DisplayerToolHOCR* displayerTool, QWidget* parent)
	: QDialog(parent), m_hocrdocument(hocrdocument), m_previewPage(previewPage), m_displayerTool(displayerTool)
{
	ui.setupUi(this);
	ui.comboBoxImageFormat->addItem(_("Color"), QImage::Format_RGB888);
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
	ui.comboBoxImageFormat->addItem(_("Grayscale"), QImage::Format_Indexed8);
#else
	ui.comboBoxImageFormat->addItem(_("Grayscale"), QImage::Format_Grayscale8);
#endif
	ui.comboBoxImageFormat->addItem(_("Monochrome"), QImage::Format_Mono);
	ui.comboBoxImageFormat->setCurrentIndex(-1);
	ui.comboBoxDithering->addItem(_("Threshold (closest color)"), Qt::ThresholdDither);
	ui.comboBoxDithering->addItem(_("Diffuse"), Qt::DiffuseDither);
	ui.comboBoxImageCompression->addItem(_("Zip (lossless)"), PDFSettings::CompressZip);
	ui.comboBoxImageCompression->addItem(_("CCITT Group 4 (lossless)"), PDFSettings::CompressFax4);
	ui.comboBoxImageCompression->addItem(_("Jpeg (lossy)"), PDFSettings::CompressJpeg);
	ui.comboBoxImageCompression->setCurrentIndex(-1);

	m_pdfFontDialog = new QFontDialog(this);

	connect(ui.buttonFont, SIGNAL(clicked()), m_pdfFontDialog, SLOT(exec()));
	connect(m_pdfFontDialog, SIGNAL(fontSelected(QFont)), this, SLOT(updateFontButton(QFont)));
	connect(ui.comboBoxOutputMode, SIGNAL(currentIndexChanged(int)), this, SLOT(updatePreview()));
	connect(ui.comboBoxImageFormat, SIGNAL(currentIndexChanged(int)), this, SLOT(updatePreview()));
	connect(ui.comboBoxImageFormat, SIGNAL(currentIndexChanged(int)), this, SLOT(imageFormatChanged()));
	connect(ui.comboBoxDithering, SIGNAL(currentIndexChanged(int)), this, SLOT(updatePreview()));
	connect(ui.comboBoxImageCompression, SIGNAL(currentIndexChanged(int)), this, SLOT(imageCompressionChanged()));
	connect(ui.spinBoxCompressionQuality, SIGNAL(valueChanged(int)), this, SLOT(updatePreview()));
	connect(ui.checkBoxFontSize, SIGNAL(toggled(bool)), this, SLOT(updatePreview()));
	connect(ui.checkBoxFontSize, SIGNAL(toggled(bool)), ui.labelFontScaling, SLOT(setEnabled(bool)));
	connect(ui.checkBoxFontSize, SIGNAL(toggled(bool)), ui.spinFontScaling, SLOT(setEnabled(bool)));
	connect(ui.spinFontScaling, SIGNAL(valueChanged(int)), this, SLOT(updatePreview()));
	connect(ui.checkBoxUniformizeSpacing, SIGNAL(toggled(bool)), this, SLOT(updatePreview()));
	connect(ui.spinBoxPreserve, SIGNAL(valueChanged(int)), this, SLOT(updatePreview()));
	connect(ui.checkBoxUniformizeSpacing, SIGNAL(toggled(bool)), ui.labelPreserve, SLOT(setEnabled(bool)));
	connect(ui.checkBoxUniformizeSpacing, SIGNAL(toggled(bool)), ui.labelPreserveCharacters, SLOT(setEnabled(bool)));
	connect(ui.checkBoxUniformizeSpacing, SIGNAL(toggled(bool)), ui.spinBoxPreserve, SLOT(setEnabled(bool)));
	connect(ui.lineEditPasswordOpen, SIGNAL(textChanged(const QString&)), this, SLOT(passwordChanged()));
	connect(ui.lineEditConfirmPasswordOpen, SIGNAL(textChanged(const QString&)), this, SLOT(passwordChanged()));
	connect(ui.checkBoxPreview, SIGNAL(toggled(bool)), this, SLOT(updatePreview()));

	MAIN->getConfig()->addSetting(new ComboSetting("pdfexportmode", ui.comboBoxOutputMode));
	MAIN->getConfig()->addSetting(new FontSetting("pdffont", m_pdfFontDialog, QFont().toString()));
	MAIN->getConfig()->addSetting(new SpinSetting("pdfimagecompressionquality", ui.spinBoxCompressionQuality, 90));
	MAIN->getConfig()->addSetting(new ComboSetting("pdfimagecompression", ui.comboBoxImageCompression));
	MAIN->getConfig()->addSetting(new ComboSetting("pdfimageformat", ui.comboBoxImageFormat));
	MAIN->getConfig()->addSetting(new ComboSetting("pdfimageconversionflags", ui.comboBoxDithering));
	MAIN->getConfig()->addSetting(new SpinSetting("pdfimagedpi", ui.spinBoxDpi, 300));
	MAIN->getConfig()->addSetting(new SwitchSetting("pdfusedetectedfontsizes", ui.checkBoxFontSize, true));
	MAIN->getConfig()->addSetting(new SpinSetting("pdffontscale", ui.spinFontScaling, 100));
	MAIN->getConfig()->addSetting(new SwitchSetting("pdfuniformizelinespacing", ui.checkBoxUniformizeSpacing, false));
	MAIN->getConfig()->addSetting(new SpinSetting("pdfpreservespaces", ui.spinBoxPreserve, 4));
	MAIN->getConfig()->addSetting(new SwitchSetting("pdfpreview", ui.checkBoxPreview, false));

#ifndef MAKE_VERSION
#define MAKE_VERSION(...) 0
#endif
#if !defined(TESSERACT_VERSION) || TESSERACT_VERSION < MAKE_VERSION(3,04,00)
	ui.checkBoxFontSize->setChecked(false);
	ui.checkBoxFontSize->setVisible(false);
	ui.spinFontScaling->setVisible(false);
	ui.labelFontScaling->setVisible(false);
#endif

	updateFontButton(m_pdfFontDialog->currentFont());
}

HOCRPdfExporter::~HOCRPdfExporter()
{
	MAIN->getConfig()->removeSetting("pdfexportmode");
	MAIN->getConfig()->removeSetting("pdffont");
	MAIN->getConfig()->removeSetting("pdfimagecompressionquality");
	MAIN->getConfig()->removeSetting("pdfimagecompression");
	MAIN->getConfig()->removeSetting("pdfimageformat");
	MAIN->getConfig()->removeSetting("pdfimagedpi");
	MAIN->getConfig()->removeSetting("pdfusedetectedfontsizes");
	MAIN->getConfig()->removeSetting("pdffontscale");
	MAIN->getConfig()->removeSetting("pdfuniformizelinespacing");
	MAIN->getConfig()->removeSetting("pdfpreservespaces");
	MAIN->getConfig()->removeSetting("pdfpreview");
}

bool HOCRPdfExporter::run(QString& filebasename) {
	m_preview = new QGraphicsPixmapItem();
	m_preview->setTransformationMode(Qt::SmoothTransformation);
	updatePreview();
	MAIN->getDisplayer()->scene()->addItem(m_preview);

	bool accepted = false;
	PoDoFo::PdfStreamedDocument* document = nullptr;
	PoDoFo::PdfFont* font = nullptr;
#if PODOFO_VERSION >= PODOFO_MAKE_VERSION(0,9,3)
	const PoDoFo::PdfEncoding* pdfEncoding = PoDoFo::PdfEncodingFactory::GlobalIdentityEncodingInstance();
#else
	const PoDoFo::PdfEncoding* pdfEncoding = new PoDoFo::PdfIdentityEncoding;
#endif
	QString outname;
	while(true) {
		accepted = (exec() == QDialog::Accepted);
		if(!accepted) {
			break;
		}

		QString suggestion = filebasename;
		if(suggestion.isEmpty()) {
			QList<Source*> sources = MAIN->getSourceManager()->getSelectedSources();
			suggestion = !sources.isEmpty() ? QFileInfo(sources.first()->displayname).baseName() : _("output");
		}

		outname = FileDialogs::saveDialog(_("Save PDF Output..."), suggestion + ".pdf", "outputdir", QString("%1 (*.pdf)").arg(_("PDF Files")));
		if(outname.isEmpty()) {
			accepted = false;
			break;
		}
		filebasename = QFileInfo(outname).completeBaseName();

		PoDoFo::PdfEncrypt* encrypt = PoDoFo::PdfEncrypt::CreatePdfEncrypt(ui.lineEditPasswordOpen->text().toStdString(),
									  ui.lineEditPasswordOpen->text().toStdString(),
									  PoDoFo::PdfEncrypt::EPdfPermissions::ePdfPermissions_Print |
									  PoDoFo::PdfEncrypt::EPdfPermissions::ePdfPermissions_Edit |
									  PoDoFo::PdfEncrypt::EPdfPermissions::ePdfPermissions_Copy |
									  PoDoFo::PdfEncrypt::EPdfPermissions::ePdfPermissions_EditNotes |
									  PoDoFo::PdfEncrypt::EPdfPermissions::ePdfPermissions_FillAndSign |
									  PoDoFo::PdfEncrypt::EPdfPermissions::ePdfPermissions_Accessible |
									  PoDoFo::PdfEncrypt::EPdfPermissions::ePdfPermissions_DocAssembly |
									  PoDoFo::PdfEncrypt::EPdfPermissions::ePdfPermissions_HighPrint,
									  PoDoFo::PdfEncrypt::EPdfEncryptAlgorithm::ePdfEncryptAlgorithm_RC4V2);
		try {

			document = new PoDoFo::PdfStreamedDocument(outname.toLocal8Bit().data(), PoDoFo::EPdfVersion::ePdfVersion_1_7, encrypt);
		} catch(...) {
			QMessageBox::critical(MAIN, _("Failed to save output"), _("Check that you have writing permissions in the selected folder."));
			continue;
		}
		try {
			QFontInfo info(m_pdfFontDialog->currentFont());
#if PODOFO_VERSION >= PODOFO_MAKE_VERSION(0,9,3)
			font = document->CreateFontSubset(info.family().toLocal8Bit().data(), info.bold(), info.italic(), false, pdfEncoding);
#else
			font = document->CreateFontSubset(info.family().toLocal8Bit().data(), info.bold(), info.italic(), pdfEncoding);
#endif
		} catch(...) {
			font = nullptr;
		}
		if(!font) {
			QMessageBox::critical(MAIN, _("Error"), _("The PDF library does not support the selected font."));
			document->Close();
			delete document;
			continue;
		}
		break;
	}

	MAIN->getDisplayer()->scene()->removeItem(m_preview);
	delete m_preview;
	m_preview = nullptr;
	if(!accepted) {
		return false;
	}

	PoDoFo::PdfPainter painter;

	PDFSettings pdfSettings;
	pdfSettings.colorFormat = static_cast<QImage::Format>(ui.comboBoxImageFormat->itemData(ui.comboBoxImageFormat->currentIndex()).toInt());
	pdfSettings.conversionFlags = pdfSettings.colorFormat == QImage::Format_Mono ? static_cast<Qt::ImageConversionFlags>(ui.comboBoxDithering->itemData(ui.comboBoxDithering->currentIndex()).toInt()) : Qt::AutoColor;
	pdfSettings.compression = static_cast<PDFSettings::Compression>(ui.comboBoxImageCompression->itemData(ui.comboBoxImageCompression->currentIndex()).toInt());
	pdfSettings.compressionQuality = ui.spinBoxCompressionQuality->value();
	pdfSettings.useDetectedFontSizes = ui.checkBoxFontSize->isChecked();
	pdfSettings.uniformizeLineSpacing = ui.checkBoxUniformizeSpacing->isChecked();
	pdfSettings.preserveSpaceWidth = ui.spinBoxPreserve->value();
	pdfSettings.overlay = ui.comboBoxOutputMode->currentIndex() == 1;
	pdfSettings.detectedFontScaling = ui.spinFontScaling->value() / 100.;
	QStringList failed;
	for(int i = 0, n = m_hocrdocument->pageCount(); i < n; ++i) {
		const HOCRPage* page = m_hocrdocument->page(i);
		if(!page->isEnabled()) {
			continue;
		}
		QRect bbox = page->bbox();
		int sourceDpi = page->resolution();
		int outputDpi = ui.spinBoxDpi->value();
		if(MAIN->getSourceManager()->addSource(page->sourceFile())) {
			MAIN->getDisplayer()->setup(&page->pageNr(), &outputDpi, &page->angle());
			double docScale = (72. / sourceDpi);
			double imgScale = double(outputDpi) / sourceDpi;
			PoDoFo::PdfPage* pdfpage = document->CreatePage(PoDoFo::PdfRect(0, 0, bbox.width() * docScale, bbox.height() * docScale));
			painter.SetPage(pdfpage);
			painter.SetFont(font);

			PoDoFoPDFPainter pdfprinter(document, &painter, docScale);
			pdfprinter.setFontSize(m_pdfFontDialog->currentFont().pointSize());
			printChildren(pdfprinter, page, pdfSettings, imgScale);
			if(pdfSettings.overlay) {
				QRect scaledBBox(imgScale * bbox.left(), imgScale * bbox.top(), imgScale * bbox.width(), imgScale * bbox.height());
				pdfprinter.drawImage(bbox, m_displayerTool->getSelection(scaledBBox), pdfSettings);
			}
			MAIN->getDisplayer()->setup(nullptr, &sourceDpi);
			painter.FinishPage();
		} else {
			failed.append(page->title());
		}
	}
	if(!failed.isEmpty()) {
		QMessageBox::warning(MAIN, _("Errors occurred"), _("The following pages could not be rendered:\n%1").arg(failed.join("\n")));
	}

	bool pdfCanBeOpened = true;
	try {
		document->Close();
	} catch(PoDoFo::PdfError& e) {
		pdfCanBeOpened = false;
		QMessageBox::warning(MAIN, _("Export failed"), _("The PDF export failed (%1).").arg(e.what()));
	}
	if(ui.checkBoxOpenOutputPdf->isChecked() && pdfCanBeOpened) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(outname));
	}

	delete document;
	return pdfCanBeOpened;
}

void HOCRPdfExporter::printChildren(PDFPainter& painter, const HOCRItem* item, const PDFSettings& pdfSettings, double imgScale) const {
	if(!item->isEnabled()) {
		return;
	}
	QString itemClass = item->itemClass();
	QRect itemRect = item->bbox();
	int childCount = item->children().size();
	if(itemClass == "ocr_par" && pdfSettings.uniformizeLineSpacing) {
		double yInc = double(itemRect.height()) / childCount;
		double y = itemRect.top() + yInc;
		int baseline = childCount > 0 ? item->children()[0]->baseLine() : 0;
		for(int iLine = 0; iLine < childCount; ++iLine, y += yInc) {
			HOCRItem* lineItem = item->children()[iLine];
			int x = itemRect.x();
			int prevWordRight = itemRect.x();
			for(int iWord = 0, nWords = lineItem->children().size(); iWord < nWords; ++iWord) {
				HOCRItem* wordItem = lineItem->children()[iWord];
				if(!wordItem->isEnabled()) {
					continue;
				}
				QRect wordRect = wordItem->bbox();
				if(pdfSettings.useDetectedFontSizes) {
					painter.setFontSize(wordItem->fontSize() * pdfSettings.detectedFontScaling);
				}
				// If distance from previous word is large, keep the space
				if(wordRect.x() - prevWordRight > pdfSettings.preserveSpaceWidth * painter.getAverageCharWidth()) {
					x = wordRect.x();
				}
				prevWordRight = wordRect.right();
				QString text = wordItem->text();
				painter.drawText(x, y + baseline, text);
				x += painter.getTextWidth(text + " ");
			}
		}
	} else if(itemClass == "ocr_line" && !pdfSettings.uniformizeLineSpacing) {
		int baseline = item->baseLine();
		double y = itemRect.bottom() + baseline;
		for(int iWord = 0, nWords = item->children().size(); iWord < nWords; ++iWord) {
			HOCRItem* wordItem = item->children()[iWord];
			QRect wordRect = wordItem->bbox();
			if(pdfSettings.useDetectedFontSizes) {
				painter.setFontSize(wordItem->fontSize() * pdfSettings.detectedFontScaling);
			}
			painter.drawText(wordRect.x(), y, wordItem->text());
		}
	} else if(itemClass == "ocr_graphic" && !pdfSettings.overlay) {
		QRect scaledItemRect(itemRect.left() * imgScale, itemRect.top() * imgScale, itemRect.width() * imgScale, itemRect.height() * imgScale);
		painter.drawImage(itemRect, m_displayerTool->getSelection(scaledItemRect), pdfSettings);
	} else {
		for(int i = 0, n = item->children().size(); i < n; ++i) {
			printChildren(painter, item->children()[i], pdfSettings, imgScale);
		}
	}
}

void HOCRPdfExporter::updatePreview() {
	if(!m_preview) {
		return;
	}
	m_preview->setVisible(ui.checkBoxPreview->isChecked());
	if(m_hocrdocument->pageCount() == 0 || !ui.checkBoxPreview->isChecked()) {
		return;
	}
	const HOCRPage* page = m_previewPage;
	QRect bbox = page->bbox();
	int pageDpi = page->resolution();

	PDFSettings pdfSettings;
	pdfSettings.colorFormat = static_cast<QImage::Format>(ui.comboBoxImageFormat->itemData(ui.comboBoxImageFormat->currentIndex()).toInt());
	pdfSettings.conversionFlags = pdfSettings.colorFormat == QImage::Format_Mono ? static_cast<Qt::ImageConversionFlags>(ui.comboBoxDithering->itemData(ui.comboBoxDithering->currentIndex()).toInt()) : Qt::AutoColor;
	pdfSettings.compression = static_cast<PDFSettings::Compression>(ui.comboBoxImageCompression->itemData(ui.comboBoxImageCompression->currentIndex()).toInt());
	pdfSettings.compressionQuality = ui.spinBoxCompressionQuality->value();
	pdfSettings.useDetectedFontSizes = ui.checkBoxFontSize->isChecked();
	pdfSettings.uniformizeLineSpacing = ui.checkBoxUniformizeSpacing->isChecked();
	pdfSettings.preserveSpaceWidth = ui.spinBoxPreserve->value();
	pdfSettings.overlay = ui.comboBoxOutputMode->currentIndex() == 1;
	pdfSettings.detectedFontScaling = ui.spinFontScaling->value() / 100.;

	QImage image(bbox.size(), QImage::Format_ARGB32);
	image.setDotsPerMeterX(pageDpi / 0.0254);
	image.setDotsPerMeterY(pageDpi / 0.0254);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setFont(m_pdfFontDialog->currentFont());
	QPainterPDFPainter pdfPrinter(&painter);

	if(pdfSettings.overlay) {
		pdfPrinter.drawImage(bbox, m_displayerTool->getSelection(bbox), pdfSettings);
		painter.fillRect(0, 0, bbox.width(), bbox.height(), QColor(255, 255, 255, 127));
	} else {
		image.fill(Qt::white);
	}
	printChildren(pdfPrinter, page, pdfSettings);
	m_preview->setPixmap(QPixmap::fromImage(image));
	m_preview->setPos(-0.5 * bbox.width(), -0.5 * bbox.height());
}

void HOCRPdfExporter::imageFormatChanged() {
	QImage::Format format = static_cast<QImage::Format>(ui.comboBoxImageFormat->itemData(ui.comboBoxImageFormat->currentIndex()).toInt());
	QStandardItemModel* model = static_cast<QStandardItemModel*>(ui.comboBoxImageCompression->model());
	int zipIdx = ui.comboBoxImageCompression->findData(PDFSettings::CompressZip);
	int ccittIdx = ui.comboBoxImageCompression->findData(PDFSettings::CompressFax4);
	int jpegIdx = ui.comboBoxImageCompression->findData(PDFSettings::CompressJpeg);
	QStandardItem* ccittItem = model->item(ccittIdx);
	QStandardItem* jpegItem = model->item(jpegIdx);
	if(format == QImage::Format_Mono) {
		if(ui.comboBoxImageCompression->currentIndex() == jpegIdx) {
			ui.comboBoxImageCompression->setCurrentIndex(zipIdx);
		}
		ccittItem->setFlags(ccittItem->flags()|Qt::ItemIsSelectable|Qt::ItemIsEnabled);
		jpegItem->setFlags(jpegItem->flags() & ~(Qt::ItemIsSelectable|Qt::ItemIsEnabled));
		ui.labelDithering->setEnabled(true);
		ui.comboBoxDithering->setEnabled(true);
	} else {
		if(ui.comboBoxImageCompression->currentIndex() == ccittIdx) {
			ui.comboBoxImageCompression->setCurrentIndex(zipIdx);
		}
		ccittItem->setFlags(ccittItem->flags() & ~(Qt::ItemIsSelectable|Qt::ItemIsEnabled));
		jpegItem->setFlags(jpegItem->flags()|Qt::ItemIsSelectable|Qt::ItemIsEnabled);
		ui.labelDithering->setEnabled(false);
		ui.comboBoxDithering->setEnabled(false);
	}
}

void HOCRPdfExporter::imageCompressionChanged() {
	PDFSettings::Compression compression = static_cast<PDFSettings::Compression>(ui.comboBoxImageCompression->itemData(ui.comboBoxImageCompression->currentIndex()).toInt());
	bool jpegCompression = compression == PDFSettings::CompressJpeg;
	ui.spinBoxCompressionQuality->setEnabled(jpegCompression);
	ui.labelCompressionQuality->setEnabled(jpegCompression);
}

void HOCRPdfExporter::passwordChanged() {
	if(ui.lineEditPasswordOpen->text() ==
			ui.lineEditConfirmPasswordOpen->text()) {

		ui.lineEditConfirmPasswordOpen->setStyleSheet(QStringLiteral(""));
		ui.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
	} else {
		ui.lineEditConfirmPasswordOpen->setStyleSheet("background: #FF7777; color: #FFFFFF;");
		ui.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
	}
}

void HOCRPdfExporter::updateFontButton(const QFont& font) {
	ui.buttonFont->setText(QString("%1 %2").arg(font.family()).arg(font.pointSize()));
	updatePreview();
}
