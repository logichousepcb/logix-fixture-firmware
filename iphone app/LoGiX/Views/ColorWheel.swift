import SwiftUI

/// Hue/saturation wheel. Brightness is a separate control, so the wheel always produces full-value colors.
struct ColorWheel: View {
    let color: RGB
    let onChange: (RGB) -> Void

    @State private var hue: Double = 0
    @State private var sat: Double = 0
    @State private var dragging = false

    var body: some View {
        GeometryReader { geo in
            let size = min(geo.size.width, geo.size.height)
            let radius = size / 2
            let center = CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)
            let angle = hue * 2 * .pi
            let thumb = CGPoint(x: center.x + cos(angle) * sat * radius,
                                y: center.y + sin(angle) * sat * radius)

            ZStack {
                Circle()
                    .fill(AngularGradient(colors: stride(from: 0.0, through: 1.0, by: 1.0 / 12).map {
                        Color(hue: $0, saturation: 1, brightness: 1)
                    }, center: .center, startAngle: .degrees(0), endAngle: .degrees(360)))
                Circle()
                    .fill(RadialGradient(colors: [.white, .white.opacity(0)], center: .center,
                                         startRadius: 0, endRadius: radius))
                Circle().stroke(Theme.line, lineWidth: 1)
            }
            .frame(width: size, height: size)
            .position(center)

            Circle()
                .fill(RGB.fromHSV(h: hue, s: sat).color)
                .frame(width: dragging ? 38 : 30, height: dragging ? 38 : 30)
                .overlay(Circle().stroke(.white, lineWidth: 3))
                .shadow(color: .black.opacity(0.5), radius: 4)
                .position(thumb)
                .animation(.spring(duration: 0.2), value: dragging)
                .allowsHitTesting(false)

            Color.clear
                .contentShape(Circle().size(width: size, height: size).offset(x: center.x - radius, y: center.y - radius))
                .gesture(DragGesture(minimumDistance: 0).onChanged { v in
                    dragging = true
                    let dx = v.location.x - center.x, dy = v.location.y - center.y
                    var a = atan2(dy, dx)
                    if a < 0 { a += 2 * .pi }
                    hue = a / (2 * .pi)
                    sat = min(1, sqrt(dx * dx + dy * dy) / radius)
                    onChange(RGB.fromHSV(h: hue, s: sat))
                }.onEnded { _ in
                    dragging = false
                })
        }
        .aspectRatio(1, contentMode: .fit)
        .sensoryFeedback(.selection, trigger: dragging)
        .onAppear { sync(color) }
        .onChange(of: color) { _, c in if !dragging { sync(c) } }
    }

    private func sync(_ c: RGB) {
        let hs = c.hueSaturation
        // keep the hue where it is for whites/greys so the thumb doesn't jump
        if hs.s > 0.01 { hue = hs.h }
        sat = hs.s
    }
}
