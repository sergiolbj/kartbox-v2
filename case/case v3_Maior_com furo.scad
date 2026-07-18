/* ====================================================================
   CASE PARAMÉTRICO COMPACTO KARTBOX (V35 - COLUNAS MACIÇAS REFORÇADAS)
   ==================================================================== */

$fn = 60; 

// ====================================================================
// 1. CONTROLES DE CRESCIMENTO E TAMANHO GERAL
// ====================================================================
extensao_horizontal_x = 4.00;  
extensao_vertical_y   = 0.00;  
caixa_profundidade_interna = 21.50; 
parede_espessura      = 2.00; 
raio_canto_externo    = 4.50; 

// ====================================================================
// 2. CONFIGURAÇÕES FIXAS DOS COMPONENTES (VISOR, BOTÕES, GPS, ÍMÃS, USB)
// ====================================================================
visor_vidro_x     = 114.40; 
visor_vidro_y     = 66.80;  
visor_espessura   = 2.00; 
visor_ativa_x     = 93.60;  
visor_ativa_y     = 56.16;  
folga_visor       = 0.30; 

botao_pino_diametro = 4.20; 
botao_corpo_medida  = 6.40; 
botao_profundidade  = 1.50; 
qtd_botoes          = 4;     
margin_botoes       = 6.50; 

gps_x                = 16.00; 
gps_y                = 12.00; 
folga_gps            = 0.40;  
gps_shift_x          = 0.00; 

ima_x                = 30.00; 
ima_y                = 10.00; 
ima_z                = 0.10;  
folga_ima            = 0.05;  
dist_lateral_ima     = 12.00; 

// CONFIGURAÇÃO DO INSERT DE LATÃO M3 (CAIXA VERDE)
diametro_furo_insert = 4.20; 

// CONFIGURAÇÕES PARAMÉTRICAS DO PARAFUSO ALLEN DIN 912 M3 (MOLDURA BRANCA)
parafuso_passante_diametro = 3.40; 
rebaixo_cabeca_diametro    = 6.50; 
rebaixo_cabeca_profundidade = 3.00; 

// CONFIGURAÇÕES PARAMÉTRICAS DO CONECTOR USB-C
usbc_largura    = 9;  
usbc_altura     = 3.30;  
folga_usbc_furo = 0.20;  
usbc_shift_y    = 0.00;  // AJUSTE HORIZONTAL: (+) move para frente, (-) move para trás
usbc_shift_z    = -8.30;  // AJUSTE VERTICAL:   (+) move para cima,   (-) move para baixo

// ====================================================================
// 3. MATEMÁTICA AUTOMÁTICA
// ====================================================================
janela_x = visor_ativa_x + 0.4; 
janela_y = visor_ativa_y + 0.4;
berco_x  = visor_vidro_x + folga_visor;
berco_y  = visor_vidro_y + folga_visor;

borda_lateral_min = 1.50;

caixa_externa_x = berco_x + (borda_lateral_min * 2) + (extensao_horizontal_x * 2);
caixa_externa_y = berco_y + (parede_espessura * 2) + margin_botoes + (extensao_vertical_y * 2); 

espessura_base_traseira = ima_z + 1.20; 
caixa_profundidade      = caixa_profundidade_interna + espessura_base_traseira;

dist_furo   = 4.2;
pos_furos_x = [dist_furo, caixa_externa_x - dist_furo];
pos_furos_y = [dist_furo, caixa_externa_y - dist_furo];

pos_x_visor = (caixa_externa_x - berco_x) / 2;
pos_y_visor = margin_botoes + parede_espessura + extensao_vertical_y;

dim_gps_x      = gps_x + folga_gps;
dim_gps_y      = gps_y + folga_gps;
pos_x_gps_base = (caixa_externa_x - dim_gps_x) / 2 + gps_shift_x;
pos_y_gps_base = caixa_externa_y - parede_espessura - dim_gps_y;

dim_ima_x = ima_x + folga_ima;
dim_ima_y = ima_y + folga_ima;
dim_ima_z = ima_z + 0.15;

dim_furo_usb_x = parede_espessura + 2.0; 
dim_furo_usb_y = usbc_largura + folga_usbc_furo;
dim_furo_usb_z = usbc_altura + folga_usbc_furo;

pos_z_usb = (espessura_base_traseira + (caixa_profundidade_interna - dim_furo_usb_z) / 2) + usbc_shift_z;
pos_y_usb = ((caixa_externa_y - dim_furo_usb_y) / 2) + usbc_shift_y;

/* ====================================================================
   MÓDULOS AUXILIARES DE GEOMETRIA (HULL)
   ==================================================================== */

module bloco_arredondado_2d(x, y, z, r) {
    hull() {
        translate([r, r, 0]) cylinder(r = r, h = z);
        translate([x - r, r, 0]) cylinder(r = r, h = z);
        translate([r, y - r, 0]) cylinder(r = r, h = z);
        translate([x - r, y - r, 0]) cylinder(r = r, h = z);
    }
}

module bloco_traseiro_moderno(x, y, z, r) {
    r_fundo = 1.5; 
    hull() {
        translate([r, r, r_fundo]) cylinder(r = r, h = z - r_fundo);
        translate([x - r, r, r_fundo]) cylinder(r = r, h = z - r_fundo);
        translate([r, y - r, r_fundo]) cylinder(r = r, h = z - r_fundo);
        translate([x - r, y - r, r_fundo]) cylinder(r = r, h = z - r_fundo);
        
        translate([r, r, r_fundo]) sphere(r = r_fundo);
        translate([x - r, r, r_fundo]) sphere(r = r_fundo);
        translate([r, y - r, r_fundo]) sphere(r = r_fundo);
        translate([x - r, y - r, r_fundo]) sphere(r = r_fundo);
    }
}

module modulo_furo_usbc_arredondado(comprimento_corte, largura, altura) {
    raio_canto = altura / 2;
    rotate([0, 90, 0])
    hull() {
        translate([-raio_canto, raio_canto, 0]) 
            cylinder(r = raio_canto, h = comprimento_corte);
        translate([-raio_canto, largura - raio_canto, 0]) 
            cylinder(r = raio_canto, h = comprimento_corte);
    }
}

/* ====================================================================
   PEÇAS COMPONENTES DO SANDUÍCHE
   ==================================================================== */

// 1. MOLDURA FRONTAL - PEÇA BRANCA
module moldura_frontal() {
    espessura_moldura = 3.50; 
    
    difference() {
        bloco_arredondado_2d(caixa_externa_x, caixa_externa_y, espessura_moldura, raio_canto_externo);
        
        translate([(caixa_externa_x - janela_x)/2, pos_y_visor + (berco_y - janela_y)/2, -1])
            cube([janela_x, janela_y, espessura_moldura + 2]);
        
        translate([pos_x_visor, pos_y_visor, -0.1])
            cube([berco_x, berco_y, visor_espessura + 0.1]);
            
        espacamento_botoes = caixa_externa_x / (qtd_botoes + 1);
        pos_y_botoes = 5.50 + extensao_vertical_y; 
        
        for (i = [1 : qtd_botoes]) {
            translate([espacamento_botoes * i, pos_y_botoes, -1])
                cylinder(d = botao_pino_diametro, h = espessura_moldura + 2);
                
            translate([(espacamento_botoes * i) - botao_corpo_medida/2, pos_y_botoes - botao_corpo_medida/2, -0.1])
                cube([botao_corpo_medida, botao_corpo_medida, botao_profundidade + 0.1]);
        }
        
        for(x = pos_furos_x) {
            for(y = pos_furos_y) {
                translate([x, y, -1]) 
                    cylinder(d = parafuso_passante_diametro, h = espessura_moldura + 2);
                
                translate([x, y, espessura_moldura - rebaixo_cabeca_profundidade]) 
                    cylinder(d = rebaixo_cabeca_diametro, h = rebaixo_cabeca_profundidade + 0.1);
            }
        }
    }
}

// 2. BERÇO INTERMEDIÁRIO - PEÇA AMARELA
module berco_intermediario() {
    espessura_berco = 3.00; 
    
    difference() {
        bloco_arredondado_2d(caixa_externa_x, caixa_externa_y, espessura_berco, raio_canto_externo);
            
        vazado_interno_x = berco_x - 6.00;
        vazado_interno_y = berco_y - 6.00;
        translate([pos_x_visor + 3.00, pos_y_visor + 3.00, -1])
            cube([vazado_interno_x, vazado_interno_y, espessura_berco + 2]);
            
        espacamento_botoes = caixa_externa_x / (qtd_botoes + 1);
        pos_y_botoes = 5.50 + extensao_vertical_y; 
        largura_furo_botao = botao_corpo_medida + 1.5; 
        
        for (i = [1 : qtd_botoes]) {
            translate([(espacamento_botoes * i) - largura_furo_botao/2, pos_y_botoes - largura_furo_botao/2, -1])
                cube([largura_furo_botao, largura_furo_botao, espessura_berco + 2]);
        }
            
        for(x = pos_furos_x, y = pos_furos_y) {
            translate([x, y, -1]) cylinder(d=3.2, h=espessura_berco + 2);
        }
    }
}

// 3. CAIXA TRASEIRA - PEÇA VERDE (CORREÇÃO DE COLUNAS COMPLETAMENTE MACIÇAS)
module caixa_traseira() {
    // REFORÇO: Raio cilíndrico interno da coluna aumentado para garantir parede grossa e fechada ao redor do insert
    raio_coluna_interna = diametro_furo_insert / 2 + 2.40; // Garante ~2.4mm de parede sólida de plástico
    
    difference() {
        // --- 1. SÓLIDO PRINCIPAL UNIFICADO ---
        union() {
            difference() {
                bloco_traseiro_moderno(caixa_externa_x, caixa_externa_y, caixa_profundidade, raio_canto_externo);
                
                // Cavidade interna das placas
                translate([parede_espessura, parede_espessura, espessura_base_traseira])
                    bloco_arredondado_2d(caixa_externa_x - (parede_espessura*2), 
                                         caixa_externa_y - (parede_espessura*2), 
                                         caixa_profundidade_interna + 1, raio_canto_externo - parede_espessura);
                                         
                // Alojamento do GPS
                translate([pos_x_gps_base, pos_y_gps_base, espessura_base_traseira])
                    cube([dim_gps_x, dim_gps_y, caixa_profundidade_interna + 1]);
                    
                // Cavidades dos ímãs
                pos_y_imas = (caixa_externa_y - dim_ima_y) / 2; 
                z_start_ima = 0.80; 
                
                translate([dist_lateral_ima + extensao_horizontal_x, pos_y_imas, z_start_ima])
                    cube([dim_ima_x, dim_ima_y, dim_ima_z]);
                    
                translate([caixa_externa_x - (dist_lateral_ima + extensao_horizontal_x) - dim_ima_x, pos_y_imas, z_start_ima])
                    cube([dim_ima_x, dim_ima_y, dim_ima_z]);
            }
            
            // ADICIONADO: Colunas cilíndricas robustas fundidas nos 4 cantos internos de Z=0 ao topo
            for(x = pos_furos_x) {
                for(y = pos_furos_y) {
                    intersection() {
                        // Mantém as colunas recortadas perfeitamente pelo design do contorno externo
                        bloco_traseiro_moderno(caixa_externa_x, caixa_externa_y, caixa_profundidade, raio_canto_externo);
                        // Cria cilindros de reforço com massa extra ao redor de cada furo
                        translate([x, y, 0])
                            cylinder(r = raio_coluna_interna, h = caixa_profundidade);
                    }
                }
            }
        }
        
        // --- 2. CORTES FINAIS SOBRE O SÓLIDO JÁ MACIÇO ---
        
        // A) Furos cegos para os inserts de latão (param em Z=1.20, protegendo o fundo)
        for(x = pos_furos_x) {
            for(y = pos_furos_y) {
                translate([x, y, 1.20]) 
                    cylinder(d = diametro_furo_insert, h = caixa_profundidade + 1);
            }
        }
        
        // B) Rasgo do USB-C Arredondado
        translate([caixa_externa_x - parede_espessura - 0.5, pos_y_usb, pos_z_usb])
            modulo_furo_usbc_arredondado(dim_furo_usb_x + 1.0, dim_furo_usb_y, dim_furo_usb_z);
    }
}

/* ====================================================================
   RENDERIZAÇÃO DO CONJUNTO
   ==================================================================== */

translate([0, 0, caixa_profundidade + 10]) color("white") moldura_frontal();
translate([0, 0, caixa_profundidade + 3]) color("yellow") berco_intermediario();
color("green") caixa_traseira();