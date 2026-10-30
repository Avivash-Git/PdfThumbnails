// The PDFium functions the handler binds at run time, in binding order:
//   PDFT_FN(<exported name>, <member of struct Pdfium>)
// Define PDFT_FN before including this file. The gate.pdfium.binds test reads this list too:
// it requires exactly 18 entries, each exported by the staged PDFium DLL. Adding or removing
// a function means changing that count in cmake/Gates.cmake as well.
// clang-format off
PDFT_FN(FPDF_InitLibraryWithConfig, InitLibraryWithConfig)
PDFT_FN(FPDF_LoadMemDocument64, LoadMemDocument64)
PDFT_FN(FPDF_CloseDocument, CloseDocument)
PDFT_FN(FPDF_GetFormType, GetFormType)
PDFT_FN(FPDF_GetPageCount, GetPageCount)
PDFT_FN(FPDF_GetPageSizeByIndexF, GetPageSizeByIndexF)
PDFT_FN(FPDF_LoadPage, LoadPage)
PDFT_FN(FPDF_ClosePage, ClosePage)
PDFT_FN(FPDFBitmap_CreateEx, BitmapCreateEx)
PDFT_FN(FPDFBitmap_FillRect, BitmapFillRect)
PDFT_FN(FPDFBitmap_Destroy, BitmapDestroy)
PDFT_FN(FPDF_RenderPageBitmap_Start, RenderPageBitmapStart)
PDFT_FN(FPDF_RenderPage_Close, RenderPageClose)
PDFT_FN(FPDFDOC_InitFormFillEnvironment, InitFormFillEnvironment)
PDFT_FN(FPDFDOC_ExitFormFillEnvironment, ExitFormFillEnvironment)
PDFT_FN(FORM_OnAfterLoadPage, FormOnAfterLoadPage)
PDFT_FN(FORM_OnBeforeClosePage, FormOnBeforeClosePage)
PDFT_FN(FPDF_FFLDraw, FFLDraw)
// clang-format on
