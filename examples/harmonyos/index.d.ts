// 此薄绑定只借用紧凑 RGBA8。Promise 完成前，调用方不得写入或 detach 输入及其别名。
// 非原生可外借的 PixelMap/图形缓冲仍需平台专用适配，不能把复制后的 ArrayBuffer 称为零复制采集。
export const encodeRgba: (pixels: ArrayBuffer, width: number, height: number) => Promise<ArrayBuffer>;
